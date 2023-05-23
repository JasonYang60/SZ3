//
// Created by Kai Zhao on 1/25/23.
//

#include "SZ3/api/sz.hpp"
#include "SZ3/utils/mddata.hpp"

using namespace SZ;

template<class T, uint N>
class Regression {
public:
    Regression(uint block_size, double eb) : quantizer_independent(eb / (N + 1)),
                                             quantizer_liner(eb / (N + 1) / block_size),
                                             prev_coeffs{0}, current_coeffs{0} {
    }

    void pred_and_quantize_coefficients() {
        for (int i = 0; i < N; i++) {
            regression_coeff_quant_inds.push_back(quantizer_liner.quantize_and_overwrite(current_coeffs[i], prev_coeffs[i]));
        }
        regression_coeff_quant_inds.push_back(
                quantizer_independent.quantize_and_overwrite(current_coeffs[N], prev_coeffs[N]));
    }

    void pred_and_recover_coefficients() {
        for (int i = 0; i < N; i++) {
            current_coeffs[i] = quantizer_liner.recover(current_coeffs[i],
                                                        regression_coeff_quant_inds[regression_coeff_index++]);
        }
        current_coeffs[N] = quantizer_independent.recover(current_coeffs[N],
                                                          regression_coeff_quant_inds[regression_coeff_index++]);

    }

    LinearQuantizer<T> quantizer_liner, quantizer_independent;
    std::vector<int> regression_coeff_quant_inds;
    size_t regression_coeff_index = 0;
    std::array<T, N + 1> current_coeffs;
    std::array<T, N + 1> prev_coeffs;

    void load(const uchar *&c, size_t &remaining_length) {
        //TODO: adjust remaining_length
        c += sizeof(uint8_t);
        remaining_length -= sizeof(uint8_t);

        size_t coeff_size = *reinterpret_cast<const size_t *>(c);
        c += sizeof(size_t);
        remaining_length -= sizeof(size_t);
        if (coeff_size != 0) {

            quantizer_independent.load(c, remaining_length);
            quantizer_liner.load(c, remaining_length);
            HuffmanEncoder<int> encoder = HuffmanEncoder<int>();
            encoder.load(c, remaining_length);
            regression_coeff_quant_inds = encoder.decode(c, coeff_size);
            encoder.postprocess_decode();
            remaining_length -= coeff_size * sizeof(int);
            std::fill(current_coeffs.begin(), current_coeffs.end(), 0);
            regression_coeff_index = 0;
        }
    }

    void save(uchar *&c) const {

        c[0] = 0b00000010;
        c += sizeof(uint8_t);
        *reinterpret_cast<size_t *>(c) = regression_coeff_quant_inds.size();
        c += sizeof(size_t);
        if (!regression_coeff_quant_inds.empty()) {
            quantizer_independent.save(c);
            quantizer_liner.save(c);
            HuffmanEncoder<int> encoder = HuffmanEncoder<int>();
            encoder.preprocess_encode(regression_coeff_quant_inds, 0);
            encoder.save(c);
            encoder.encode(regression_coeff_quant_inds, c);
            encoder.postprocess_encode();
        }
    }

    uchar *compress(Config &conf, T *data, size_t &compressed_size) {

        std::vector<int> quant_inds;
        quant_inds.reserve(conf.num);
        LinearQuantizer<T> quantizer(conf.absErrorBound);

        //=====================================================================================

        auto mddata = std::make_shared<SZ::multi_dimensional_data<T, N>>(data, conf.dims);
        auto block = mddata->block_iter(conf.blockSize);
        do {
            auto range = block->get_block_range();
            auto d = block->mddata;
            auto ds = mddata->get_dim_strides();

//            auto dims = mddata->get_dims();
            if (N == 3) {
                //preprocess
                T *cur_data_pos = d->get_data(range[0].first, range[1].first, range[2].first);
                double fx = 0.0, fy = 0.0, fz = 0.0, f = 0, sum_x, sum_y;
                int size_x = range[0].second - range[0].first;
                int size_y = range[1].second - range[1].first;
                int size_z = range[2].second - range[2].first;
                if (size_x <= 1 || size_y <= 1 || size_z <= 1) {
                    throw std::invalid_argument("Regression does not support block length in any dimension equal to 1");
                }
                for (int i = 0; i < size_x; i++) {
                    sum_x = 0;
                    for (int j = 0; j < size_y; j++) {
                        sum_y = 0;
                        for (int k = 0; k < size_z; k++) {
                            T curData = *cur_data_pos;
                            sum_y += curData;
                            fz += curData * k;
                            cur_data_pos++;
                        }
                        fy += sum_y * j;
                        sum_x += sum_y;
                        cur_data_pos += (ds[1] - size_z);
                    }
                    fx += sum_x * i;
                    f += sum_x;
                    cur_data_pos += (ds[0] - size_y * ds[1]);
                }
                double coeff = 1.0 / (size_x * size_y * size_z);
                current_coeffs[0] = (2 * fx / (size_x - 1) - f) * 6 * coeff / (size_x + 1);
                current_coeffs[1] = (2 * fy / (size_y - 1) - f) * 6 * coeff / (size_y + 1);
                current_coeffs[2] = (2 * fz / (size_z - 1) - f) * 6 * coeff / (size_z + 1);
                current_coeffs[3] = f * coeff - ((size_x - 1) * current_coeffs[0] / 2 + (size_y - 1) * current_coeffs[1] / 2 +
                                                 (size_z - 1) * current_coeffs[2] / 2);

                //preprocess_commit
                pred_and_quantize_coefficients();
                std::copy(current_coeffs.begin(), current_coeffs.end(), prev_coeffs.begin());


                //predict
                for (size_t i = 0; i < range[0].second - range[0].first; i++) {
                    for (size_t j = 0; j < range[1].second - range[1].first; j++) {
                        for (size_t k = 0; k < range[2].second - range[2].first; k++) {
                            T pred = current_coeffs[0] * i + current_coeffs[1] * j + current_coeffs[2] * k + current_coeffs[3];
                            T *c = d->get_data(i + range[0].first, j + range[1].first, k + range[2].first);
                            quant_inds.push_back(quantizer.quantize_and_overwrite(*c, pred));
                        }
                    }
                }
            }
        } while (block->next());


        HuffmanEncoder<int> encoder;
        encoder.preprocess_encode(quant_inds, 0);
        size_t bufferSize = 1.2 * (encoder.size_est() + sizeof(T) * quant_inds.size() + quantizer.size_est());

        writefile("reg.quant.int32", quant_inds.data(), quant_inds.size());

        uchar *buffer = new uchar[bufferSize];
        uchar *buffer_pos = buffer;

        save(buffer_pos);
        quantizer.save(buffer_pos);

        encoder.save(buffer_pos);
        encoder.encode(quant_inds, buffer_pos);
        encoder.postprocess_encode();
        assert(buffer_pos - buffer < bufferSize);

        Lossless_zstd lossless;
        uchar *lossless_data = lossless.compress(buffer, buffer_pos - buffer, compressed_size);
        lossless.postcompress_data(buffer);

        return lossless_data;
    }

    void decompress(Config &conf, uchar const *cmpData, const size_t &cmpSize, T *decData) {
        size_t remaining_length = cmpSize;
        LinearQuantizer<T> quantizer(conf.absErrorBound);

        Lossless_zstd lossless;
        auto compressed_data = lossless.decompress(cmpData, remaining_length);
        uchar const *compressed_data_pos = compressed_data;

        load(compressed_data_pos, remaining_length);

        quantizer.load(compressed_data_pos, remaining_length);

        HuffmanEncoder<int> encoder;
        encoder.load(compressed_data_pos, remaining_length);
        auto quant_inds = encoder.decode(compressed_data_pos, conf.num);
        encoder.postprocess_decode();

        lossless.postdecompress_data(compressed_data);

        int *quant_inds_pos = &quant_inds[0];

        size_t padding = 2;
        auto mddata = std::make_shared<SZ::multi_dimensional_data<T, N>>(nullptr, conf.dims, padding);
        auto block = mddata->block_iter(conf.blockSize);
        do {
            auto range = block->get_block_range();
            auto d = block->mddata;
            auto ds = mddata->get_dim_strides();
            if (N == 3) {
                pred_and_recover_coefficients();
                for (size_t i = 0; i < range[0].second - range[0].first; i++) {
                    for (size_t j = 0; j < range[1].second - range[1].first; j++) {
                        for (size_t k = 0; k < range[2].second - range[2].first; k++) {
                            T pred = current_coeffs[0] * i + current_coeffs[1] * j + current_coeffs[2] * k + current_coeffs[3];
                            T *c = d->get_data(i + range[0].first, j + range[1].first, k + range[2].first);
                            *c = quantizer.recover(pred, *(quant_inds_pos++));
                        }
                    }
                }
            }
        } while (block->next());

        mddata->copy_data_out(decData);
    }
};

template<class T, uint N>
void estimate_compress(Config &conf, T *data) {
    conf.absErrorBound = 1e-2;
    conf.blockSize = 6;

    Regression<T, N> reg(conf.blockSize, conf.absErrorBound);
    Timer timer(true);
    size_t cmpr_size;
    std::vector<T> data_(data, data + conf.num);
    auto cmpr_data = reg.compress(conf, data_.data(), cmpr_size);
    timer.stop("compress");
    fflush(stdout);
    printf("CR= %.3f\n", conf.num * sizeof(T) * 1.0 / cmpr_size);
    fflush(stdout);

    T *decData = new T[conf.num];
    timer.start();
    reg.decompress(conf, cmpr_data, cmpr_size, decData);
    timer.stop("decompress");


    SZ::verify(data, decData, conf.num);
};

int main(int argc, char *argv[]) {

    if (argc < 4) {
        std::cout << "usage: " << argv[0] << " data_file dim dim0 .. dimn" << std::endl;
        std::cout << "example: " << argv[0] << " qmcpack.dat 3 33120 69 69" << std::endl;
        return 0;
    }

    size_t num = 0;
    auto data = SZ::readfile<float>(argv[1], num);

    int dim = atoi(argv[2]);
    if (dim == 1) {
        Config config(atoi(argv[3]));
        estimate_compress<float, 1>(config, data.get());
    } else if (dim == 2) {
        Config config(atoi(argv[3]), atoi(argv[4]));
        estimate_compress<float, 2>(config, data.get());
    } else if (dim == 3) {
        Config config(atoi(argv[3]), atoi(argv[4]), atoi(argv[5]));
        estimate_compress<float, 3>(config, data.get());
    } else if (dim == 4) {
        Config config(atoi(argv[3]), atoi(argv[4]), atoi(argv[5]), atoi(argv[6]));
        estimate_compress<float, 4>(config, data.get());
    }

}