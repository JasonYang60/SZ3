#include <algorithm>
#include <functional>
#include <numeric>
#include <unordered_map>
#include <random>
#include <vector>
#include <unordered_set>
#include "utils/FileUtil.h"
#include "utils/Timer.hpp"
#include "utils/Verification.hpp"
#include "def.hpp"
#include "quantizer/IntegerQuantizer.hpp"
#include "lossless/Lossless_zstd.hpp"
#include "lossless/Lossless_bypass.hpp"
#include "encoder/HuffmanEncoder.hpp"
#include "encoder/BypassEncoder.hpp"
#include "compressor/SZExaaltCompressor.hpp"
#include <random>
#include <sstream>

std::string src_file_name;
float relative_error_bound = 0;

/*
 *  Internal implementation of the SMAWK algorithm.
 */
template<typename T>
void _smawk(
        const std::vector<size_t> &rows,
        const std::vector<size_t> &cols,
        const std::function<T(size_t, size_t)> &lookup,
        std::vector<size_t> *result) {
    // Recursion base case
    if (rows.size() == 0) return;

    // ********************************
    // * REDUCE
    // ********************************

    std::vector<size_t> _cols;  // Stack of surviving columns
    for (size_t col : cols) {
        while (true) {
            if (_cols.size() == 0) break;
            size_t row = rows[_cols.size() - 1];
            if (lookup(row, col) >= lookup(row, _cols.back()))
                break;
            _cols.pop_back();
        }
        if (_cols.size() < rows.size())
            _cols.push_back(col);
    }

    // Call recursively on odd-indexed rows
    std::vector<size_t> odd_rows;
    for (size_t i = 1; i < rows.size(); i += 2) {
        odd_rows.push_back(rows[i]);
    }
    _smawk(odd_rows, _cols, lookup, result);

    std::unordered_map<size_t, size_t> col_idx_lookup;
    for (size_t idx = 0; idx < _cols.size(); ++idx) {
        col_idx_lookup[_cols[idx]] = idx;
    }

    // ********************************
    // * INTERPOLATE
    // ********************************

    // Fill-in even-indexed rows
    size_t start = 0;
    for (size_t r = 0; r < rows.size(); r += 2) {
        size_t row = rows[r];
        size_t stop = _cols.size() - 1;
        if (r < rows.size() - 1)
            stop = col_idx_lookup[(*result)[rows[r + 1]]];
        size_t argmin = _cols[start];
        T min = lookup(row, argmin);
        for (size_t c = start + 1; c <= stop; ++c) {
            T value = lookup(row, _cols[c]);
            if (c == start || value < min) {
                argmin = _cols[c];
                min = value;
            }
        }
        (*result)[row] = argmin;
        start = stop;
    }
}

/*
 *  Interface for the SMAWK algorithm, for finding the minimum value in each row
 *  of an implicitly-defined totally monotone matrix.
 */
template<typename T>
std::vector<size_t> smawk(
        const size_t num_rows,
        const size_t num_cols,
        const std::function<T(size_t, size_t)> &lookup) {
    std::vector<size_t> result;
    result.resize(num_rows);
    std::vector<size_t> rows(num_rows);
    iota(begin(rows), end(rows), 0);
    std::vector<size_t> cols(num_cols);
    iota(begin(cols), end(cols), 0);
    _smawk<T>(rows, cols, lookup, &result);
    return result;
}

/*
 *  Calculates cluster costs in O(1) using prefix sum arrays.
 */
template<class DT>
class CostCalculator {
    std::vector<double> cumsum;
    std::vector<double> cumsum2;

public:
    CostCalculator(const std::vector<DT> &vec, size_t n) {
        cumsum.push_back(0.0);
        cumsum2.push_back(0.0);
        for (size_t i = 0; i < n; ++i) {
            double x = vec[i];
            cumsum.push_back(x + cumsum[i]);
            cumsum2.push_back(x * x + cumsum2[i]);
        }
    }

    double calc(size_t i, size_t j) {
        if (j < i) return 0.0;
        double mu = (cumsum[j + 1] - cumsum[i]) / (j - i + 1);
        double result = cumsum2[j + 1] - cumsum2[i];
        result += (j - i + 1) * (mu * mu);
        result -= (2 * mu) * (cumsum[j + 1] - cumsum[i]);
        return result;
    }
};

template<typename T>
class Matrix {
    std::vector<T> data;
    size_t num_rows;
    size_t num_cols;

public:
    Matrix(size_t num_rows, size_t num_cols) {
        this->num_rows = num_rows;
        this->num_cols = num_cols;
        data.resize(num_rows * num_cols);
    }

    inline T get(size_t i, size_t j) {
        return data[i * num_cols + j];
    }

    inline void set(size_t i, size_t j, T value) {
        data[i * num_cols + j] = value;
    }
};

template<class DT>
void cluster(
        DT *array,
        size_t n,
        int &k,
        size_t *clusters,
        DT *centroids) {
    // ***************************************************
    // * Sort input array and save info for de-sorting
    // ***************************************************

    std::vector<size_t> sort_idxs(n);
    iota(sort_idxs.begin(), sort_idxs.end(), 0);
    sort(
            sort_idxs.begin(),
            sort_idxs.end(),
            [&array](size_t a, size_t b) { return array[a] < array[b]; });
//    vector<size_t> undo_sort_lookup(n);
    std::vector<DT> sorted_array(n);
    for (size_t i = 0; i < n; ++i) {
        sorted_array[i] = array[sort_idxs[i]];
//        undo_sort_lookup[sort_idxs[i]] = i;
    }

    // ***************************************************
    // * Set D and T using dynamic programming algorithm
    // ***************************************************

    // Algorithm as presented in section 2.2 of (Gronlund et al., 2017).

    CostCalculator cost_calculator(sorted_array, n);
    Matrix<DT> D(k, n);
    Matrix<size_t> T(k, n);

    for (size_t i = 0; i < n; ++i) {
        D.set(0, i, cost_calculator.calc(0, i));
        T.set(0, i, 0);
    }

    double ratio_avg = 0;
    bool findk = false;
    size_t bestk = 0;
    for (size_t k_ = 1; k_ < k; ++k_) {
        auto C = [&D, &k_, &cost_calculator](size_t i, size_t j) -> DT {
            size_t col = i < j - 1 ? i : j - 1;
            return D.get(k_ - 1, col) + cost_calculator.calc(j, i);
        };
        std::vector<size_t> row_argmins = smawk<DT>(n, n, C);
        for (size_t i = 0; i < row_argmins.size(); ++i) {
            size_t argmin = row_argmins[i];
            DT min = C(i, argmin);
            D.set(k_, i, min);
            T.set(k_, i, argmin);
        }
        float ratio = D.get(k_ - 1, n - 1) / D.get(k_, n - 1);
        ratio_avg = (ratio_avg * (k_ - 1) + ratio) / (k_);
//        std::cout << k_ + 1 << " , " << D.get(k_, n - 1) << " , " << ratio << " , " << ratio_avg << std::endl;
        if (ratio / ratio_avg > 1.5) {
            bestk = k_ + 1;
            findk = true;
        } else {
            if (findk) {
                break;
            }
        }
    }

    if (!findk) {
        return;
    }
    k = bestk;
    std::cout << "# groups = " << k << std::endl;

    // ***************************************************
    // * Extract cluster assignments by backtracking
    // ***************************************************

    // TODO: This step requires O(kn) memory usage due to saving the entire
    //       T matrix. However, it can be modified so that the memory usage is O(n).
    //       D and T would not need to be retained in full (D already doesn't need
    //       to be fully retained, although it currently is).
    //       Details are in section 3 of (Grønlund et al., 2017).

//    vector<DT> sorted_clusters(n);

    size_t t = n;
    size_t k_ = k - 1;
    size_t n_ = n - 1;
    // The do/while loop was used in place of:
    //   for (k_ = k - 1; k_ >= 0; --k_)
    // to avoid wraparound of an unsigned type.
    do {
        size_t t_ = t;
        t = T.get(k_, n_);
        DT centroid = 0.0;
        for (size_t i = t; i < t_; ++i) {
//            sorted_clusters[i] = k_;
            centroid += (sorted_array[i] - centroid) / (i - t + 1);
        }
        centroids[k_] = centroid;
        k_ -= 1;
        n_ = t - 1;
    } while (t > 0);

    // ***************************************************
    // * Order cluster assignments to match de-sorted
    // * ordering
    // ***************************************************

//    for (size_t i = 0; i < n; ++i) {
//        clusters[i] = sorted_clusters[undo_sort_lookup[i]];
//    }
}

template<class T>
int f(T data, T *boundary, int n, double start_position, double offset) {
    return round((data - start_position) / offset);
}

template<class T>
int f2(T data, T *boundary, int n, double start_position, double offset) {
    int low = 0, high = n; // numElems is the size of the array i.e arr.size()
    while (low != high) {
        int mid = (low + high) / 2; // Or a fancy way to avoid int overflow
        if (boundary[mid] <= data) {
            low = mid + 1;
        } else {
            high = mid;
        }
    }
    return high;
}

template<class T>
int f3(T data, T *boundary, int n, double start_position, double offset) {
    for (int i = 1; i < n; i++) {
        if (boundary[i] > data) {
            return i - 1;
        }
    }
    return n - 1;
}

unsigned int roundUpToPowerOf2(unsigned int base) {
    base -= 1;

    base = base | (base >> 1);
    base = base | (base >> 2);
    base = base | (base >> 4);
    base = base | (base >> 8);
    base = base | (base >> 16);

    return base + 1;
}

unsigned int optimize_intervals_float_1D_opt(float *oriData, size_t dataLength, uint maxRadius, double realPrecision) {
    int sampleDistance = 100;
    double predThreshold = 0.99;
    size_t i = 0, radiusIndex;
    float pred_value = 0, pred_err;
    size_t *intervals = (size_t *) malloc(maxRadius * sizeof(size_t));
    memset(intervals, 0, maxRadius * sizeof(size_t));
    size_t totalSampleSize = 0;//dataLength/confparams_cpr->sampleDistance;

    float *data_pos = oriData + 2;
    while (data_pos - oriData < dataLength) {
        totalSampleSize++;
        pred_value = data_pos[-1];
        pred_err = fabs(pred_value - *data_pos);
        radiusIndex = (unsigned long) ((pred_err / realPrecision + 1) / 2);
        if (radiusIndex >= maxRadius)
            radiusIndex = maxRadius - 1;
        intervals[radiusIndex]++;

        data_pos += sampleDistance;
    }
    //compute the appropriate number
    size_t targetCount = totalSampleSize * predThreshold;
    size_t sum = 0;
    for (i = 0; i < maxRadius; i++) {
        sum += intervals[i];
        if (sum > targetCount)
            break;
    }
    if (i >= maxRadius)
        i = maxRadius - 1;

    unsigned int accIntervals = 2 * (i + 1);
    unsigned int powerOf2 = roundUpToPowerOf2(accIntervals);

    if (powerOf2 < 32)
        powerOf2 = 32;

    free(intervals);
    return powerOf2;
}


template<typename T, uint N>
float
SZ_Compress(std::unique_ptr<T[]> const &data, SZ::Config<T, N> conf, float level_start, float level_offset,
            int level_num) {

    std::vector<T> data_(data.get(), data.get() + conf.num);

    SZ::Timer timer;
    std::cout << "****************** Compression ******************" << std::endl;

    timer.start();
    if (N == 1) {
//        conf.quant_state_num = optimize_intervals_float_1D_opt(data.data(), data.size(), conf.quant_state_num, conf.eb);
    }
    conf.quant_state_num = 1024;
    auto sz = SZ::SZ_Exaalt_Compressor(conf, SZ::LinearQuantizer<T>(conf.eb, conf.quant_state_num / 2),
                                       SZ::HuffmanEncoder<int>(), SZ::Lossless_zstd(), 0);
    sz.set_level(level_start, level_offset, level_num);

    size_t compressed_size = 0;
    std::unique_ptr<SZ::uchar[]> compressed;
    compressed.reset(sz.compress(data.get(), compressed_size));
    timer.stop("Compression");

    auto ratio = conf.num * sizeof(T) * 1.0 / compressed_size;
    std::cout << "Compression Ratio = " << ratio << std::endl;
    std::cout << "Compressed size = " << compressed_size << std::endl;

    std::random_device rd;  //Will be used to obtain a seed for the random number engine
    std::mt19937 gen(rd()); //Standard mersenne_twister_engine seeded with rd()
    std::uniform_int_distribution<> dis(0, 10000);
    std::stringstream ss;
    ss << src_file_name.substr(src_file_name.rfind('/') + 1)
       << "." << relative_error_bound << "." << dis(gen) << ".sz3";
    auto compressed_file_name = ss.str();
    SZ::writefile(compressed_file_name.c_str(), compressed.get(), compressed_size);
    std::cout << "Compressed file = " << compressed_file_name << std::endl;

    std::cout << "****************** Decompression ****************" << std::endl;
    compressed = SZ::readfile<SZ::uchar>(compressed_file_name.c_str(), compressed_size);

    timer.start();
    std::unique_ptr<T[]> dec_data;
    dec_data.reset(sz.decompress(compressed.get(), compressed_size));
    timer.stop("Decompression");
    SZ::verify<T>(data_.data(), dec_data.get(), conf.num);

    auto decompressed_file_name = compressed_file_name + ".out";
//    SZ::writefile(decompressed_file_name.c_str(), dec_data.get(), conf.num);
//    std::cout << "Decompressed file = " << decompressed_file_name << std::endl;

    return ratio;
}

int main(int argc, char **argv) {

    SZ::Timer timer;
    timer.start();

    size_t num = 0;
    auto data = SZ::readfile<float>(argv[1], num);
    auto input = data.get();
    src_file_name = argv[1];
    std::cout << "Read " << num << " elements\n";

    int dim = atoi(argv[2] + 1);
    assert(1 <= dim && dim <= 2);
    int argp = 3;
    std::vector<size_t> dims(dim);
    for (int i = 0; i < dim; i++) {
        dims[i] = atoi(argv[argp++]);
    }

    float max = data[0];
    float min = data[0];
    for (int i = 1; i < num; i++) {
        if (max < data[i]) max = data[i];
        if (min > data[i]) min = data[i];
    }
    char *eb_op = argv[argp++] + 1;
    float eb = 0;
    if (*eb_op == 'a') {
        eb = atof(argv[argp++]);
        relative_error_bound = eb / (max - min);
    } else {
        relative_error_bound = atof(argv[argp++]);
        eb = relative_error_bound * (max - min);
    }


    timer.start();
    std::vector<float> sample;
    int sample_rate = 200000;
    while (num / sample_rate < 10000) {
        sample_rate /= 2;
    }
    std::cout << "Sample Elements = " << num / sample_rate << std::endl;
    sample.reserve(num / sample_rate);
    std::random_device rd;  //Will be used to obtain a seed for the random number engine
    std::mt19937 gen(rd()); //Standard mersenne_twister_engine seeded with rd()
    std::uniform_int_distribution<> dis(0, 2 * sample_rate);
    size_t input_idx = 0;
//        for (int i = 0; i < num / sample_rate; i++) {
//            input_idx = (input_idx + dis(gen)) % num;
//            std::cout << input_idx << " ";
//            sample[i] = input[input_idx];
//        }
//        std::cout << std::endl;
    std::uniform_int_distribution<> dis2(0, num);
    std::unordered_set<size_t> sampledkeys;
    for (int i = 0; i < num / sample_rate; i++) {
        do {
            input_idx = dis2(gen);
        } while (sampledkeys.find(input_idx) != sampledkeys.end());
//            std::cout << input_idx << " ";
        sample[i] = input[input_idx];
    }
//        std::cout << std::endl;

//    std::sample(input.begin(), input.end(),
//                sample.begin(),
//                num / sample_rate,
//                std::mt19937{std::random_device{}()});

    timer.stop("random sample");
//    sample = input;
    std::vector<size_t> idx(num);

    timer.start();
    int k = 150;
    std::vector<float> cents(k);
    cluster(sample.data(), num / sample_rate, k, idx.data(), cents.data());
//    cluster(input.get(), num, 16, idx.data(), cents.data());
    timer.stop("kmeans1d");
    if (k == 150) {
        std::cout << "No clusters are found." << std::endl;
        exit(0);
    }

    std::cout << "centers : ";
    for (size_t i = 0; i < k; i++) {
        std::cout << cents[i] << " ";
    }
    std::cout << std::endl;

    std::cout << "center diff : ";
    for (size_t i = 1; i < k; i++) {
        std::cout << cents[i] - cents[i - 1] << " ";
    }
    std::cout << std::endl;

    std::vector<float> boundary(k);
    boundary[0] = std::numeric_limits<float>::min();
    for (size_t i = 1; i < k; i++) {
        boundary[i] = (cents[i - 1] + cents[i]) / 2;
    }

    float level_offset = (cents[4] - cents[0]) / 4;
    float level_start = cents[0];
    printf("start = %.3f , level_offset = %.3f\n", level_start, level_offset);
//    if (level_start > 1.1 || level_start < 0.9 || level_offset > 1.9 || level_offset < 1.7) {
//        printf("Error start = %.3f , level_offset = %.3f\n", level_start, level_offset);
//    }
    timer.start();
    for (size_t i = 0; i < num; i++) {
//        auto iter = std::lower_bound(boundary.begin(), boundary.end(), input[i]);
//        idx[i] = iter - boundary.begin();
        idx[i] = f(input[i], boundary.data(), k, level_start, level_offset);
    }
    timer.stop("id");
//    for (size_t i = 1000000; i < 1001000; i++) {
//        std::cout << input[i] << " ";
//    }
//    std::cout << std::endl;
//    for (size_t i = 1000000; i < 1001000; i++) {
//        std::cout << int(idx[i]) - int(idx[i - 1]) << " ";
//    }

    size_t cnt = 0;
    std::vector<size_t> cnts(5);
    std::cout << std::endl;
    for (size_t i = 2; i < num; i++) {
//        std::cout << int(idx[i]) - int(idx[i - 1]) << " ";
        int prev = int(idx[i - 1]) - int(idx[i - 2]);
        int current = int(idx[i]) - int(idx[i - 1]);
        if (prev == -1 && current == 1 || prev == 1 && current == -1) {
            cnt++;
        }
        if (fabs(current) <= 2) {
            cnts[current + 2]++;
        }
    }
    std::cout << num << " " << cnt << std::endl;
    for (auto cnt1:cnts) {
        std::cout << (float) cnt1 / num * 100.0 << " ";
    }
    std::cout << std::endl;

    int max_level_diff = f(max, boundary.data(), k, level_start, level_offset) -
                         f(min, boundary.data(), k, level_start, level_offset);
    printf("level = %d\n", max_level_diff);


//    level_start = -58.291; //trinity-110x
//    level_offset = 2.241; //trinity-110x
//    level_start = 0;
//    level_offset = 1.961;
    if (dim == 1) {
        SZ_Compress(data, SZ::Config<float, 1>(eb, {dims[0]}), level_start, level_offset,
                    max_level_diff);
    } else {
        SZ_Compress(data, SZ::Config<float, 2>(eb, {dims[0], dims[1]}), level_start, level_offset,
                    max_level_diff);
    }

//    }
}