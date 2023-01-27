//
// Created by Kai Zhao on 1/25/23.
//

#include "SZ3/api/sz.hpp"
#include "omp.h"

using namespace SZ;

template<class T, uint N>
void estimate_compress(Config conf, T *data) {

    std::vector<int> quant_inds_1(conf.num);
    std::vector<int> quant_inds_2(conf.num);
    std::vector<int> quant_inds_3(conf.num);
    std::vector<int> quant_inds_4(conf.num);

    {
        LinearQuantizer<T> quantizer;
        size_t quant_count = 0;

        Timer timer(true);
        auto element_range = std::make_shared<SZ::multi_dimensional_range<T, N>>(
                data, std::begin(conf.dims), std::end(conf.dims), 1, 0);
        for (auto element = element_range->begin(); element != element_range->end(); ++element) {
            quant_inds_1[quant_count++] = quantizer.quantize_and_overwrite(*element, 0);
        }
        timer.stop("iterator");
    }

    {
        LinearQuantizer<T> quantizer;

        Timer timer(true);
        auto element_range = std::make_shared<SZ::multi_dimensional_range<T, N>>(
                data, std::begin(conf.dims), std::end(conf.dims), 1, 0);
//#pragma omp declare simd and restrict
//#pragma omp simd
        for (auto element = element_range->begin(); element != element_range->end(); ++element) {
            quant_inds_2[element.get_offset()] = quantizer.quantize_and_overwrite(*element, 0);
        }
        timer.stop("simd iterator");
    }

    {
        LinearQuantizer<T> quantizer;

        Timer timer(true);
//        size_t count = 0;
        for (size_t i = 0; i < conf.dims[0]; i++) {
            for (size_t j = 0; j < conf.dims[1]; j++) {
                for (size_t k = 0; k < conf.dims[2]; k++) {
                    size_t offset = i * conf.dims[1] * conf.dims[2] + j * conf.dims[2] + k;
                    quant_inds_3[offset] = quantizer.quantize_and_overwrite(data[offset], 0);
//                    count++;
                }
            }
        }
        timer.stop("nested loop");
    }

    {
        LinearQuantizer<T> quantizer;

        Timer timer(true);
        size_t bsize = 5;
        auto blocks = std::make_shared<SZ::multi_dimensional_range<T, N>>(
                data, std::begin(conf.dims), std::end(conf.dims), bsize, 0);
        for (auto block = blocks->begin(); block != blocks->end(); ++block) {
            auto idx = block.get_global_index();
            for (size_t i = idx[0]; i < ((idx[0] + bsize >= conf.dims[0]) ? conf.dims[0] : idx[0] + bsize); i++) {
                for (size_t j = idx[1]; j < ((idx[1] + bsize >= conf.dims[1]) ? conf.dims[1] : idx[1] + bsize); j++) {
                    for (size_t k = idx[2]; k < ((idx[2] + bsize >= conf.dims[2]) ? conf.dims[2] : idx[2] + bsize); k++) {
                        size_t offset = i * conf.dims[1] * conf.dims[2] + j * conf.dims[2] + k;
                        quant_inds_4[offset] = quantizer.quantize_and_overwrite(data[offset], 0);
                    }
                }
            }
        }

        timer.stop("block iterator + nested loop");
    }

    for (size_t i = 0; i < conf.num; i++) {
        if (quant_inds_1[i] != quant_inds_2[i] || quant_inds_2[i] != quant_inds_3[i] || quant_inds_3[i] != quant_inds_4[i]) {
            printf("Mismatch at %lu\n", i);
            exit(0);
        }
    }

}

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