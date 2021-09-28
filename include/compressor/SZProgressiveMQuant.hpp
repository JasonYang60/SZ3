#ifndef _SZ_SZ_PROG_INTERPOLATION_MULTILEVEL_QUANTIZATION_HPP
#define _SZ_SZ_PROG_INTERPOLATION_MULTILEVEL_QUANTIZATION_HPP

#include "predictor/Predictor.hpp"
#include "predictor/LorenzoPredictor.hpp"
#include "quantizer/Quantizer.hpp"
#include "encoder/Encoder.hpp"
#include "lossless/Lossless.hpp"
#include "utils/Iterator.hpp"
#include "utils/MemoryUtil.hpp"
#include "utils/Config.hpp"
#include "utils/FileUtil.hpp"
#include "utils/Interpolators.hpp"
#include "utils/Timer.hpp"
#include "def.hpp"
#include <cstring>
#include <cmath>
#include <utils/ByteUtil.hpp>
#include <utils/ska_hash/unordered_map.hpp>

namespace SZ {
    template<class T, uint N, class Quantizer, class Encoder, class Lossless>
    class SZProgressiveMQuant {
    public:


        SZProgressiveMQuant(Quantizer quantizer, Encoder encoder, Lossless lossless,
                            const std::array<size_t, N> dims,
                            int interpolator,
                            int direction,
                            size_t interp_dim_limit,
                            int level_independent_,
                            size_t block_size_,
                            int level_fill_) :
                quantizer(quantizer), encoder(encoder), lossless(lossless),
                global_dimensions(dims),
                interpolators({"linear", "cubic"}),
                interpolator_id(interpolator), direction_sequence_id(direction),
                interp_dim_limit(interp_dim_limit),
                level_independent(level_independent_),
                block_size(block_size_),
                level_fill(level_fill_) {
            static_assert(std::is_base_of<concepts::QuantizerInterface<T>, Quantizer>::value,
                          "must implement the quatizer interface");
//            static_assert(std::is_base_of<concepts::EncoderInterface<>, Encoder>::value,
//                          "must implement the encoder interface");
            static_assert(std::is_base_of<concepts::LosslessInterface, Lossless>::value,
                          "must implement the lossless interface");

            assert(interp_dim_limit % 2 == 0 &&
                   "Interpolation dimension should be even numbers to avoid extrapolation");
            num_elements = 1;
            levels = -1;
            for (int i = 0; i < N; i++) {
                if (levels < ceil(log2(dims[i]))) {
                    levels = (uint) ceil(log2(dims[i]));
                }
                num_elements *= dims[i];
                global_begin[i] = 0;
                global_end[i] = global_dimensions[i] - 1;
            }

            dimension_offsets[N - 1] = 1;
            for (int i = N - 2; i >= 0; i--) {
                dimension_offsets[i] = dimension_offsets[i + 1] * global_dimensions[i + 1];
            }

            dimension_sequences = std::vector<std::array<int, N>>();
            auto sequence = std::array<int, N>();
            for (int i = 0; i < N; i++) {
                sequence[i] = i;
            }
            do {
                dimension_sequences.push_back(sequence);
            } while (std::next_permutation(sequence.begin(), sequence.end()));

        }


        T *decompress(uchar const *lossless_data, const std::vector<size_t> &lossless_size) {

            int lossless_id = 0;
            size_t remaining_length = lossless_size[lossless_id];
            retrieved_size += remaining_length;
            uchar const *data_header = lossless_data;

            read(global_dimensions.data(), N, data_header, remaining_length);
            num_elements = std::accumulate(global_dimensions.begin(), global_dimensions.end(), (size_t) 1, std::multiplies<>());

            read(interp_dim_limit, data_header, remaining_length);
            lossless_data += lossless_size[lossless_id];
            lossless_id++;

            T *dec_data = new T[num_elements];
            size_t quant_inds_count = 0;

//            check_dec = true;
            for (uint level = levels; level > level_fill && level > 1; level--) {
                Timer timer(true);
                size_t stride = 1U << (level - 1);

                lossless_decode(lossless_data, lossless_size, lossless_id++);

                if (level == levels) {
                    *dec_data = quantizer.recover(0, quant_inds[quant_index++]);
                }

                auto block_range = std::make_shared<
                        SZ::multi_dimensional_range<T, N>>(dec_data,
                                                           std::begin(global_dimensions), std::end(global_dimensions),
                                                           stride * interp_dim_limit, 0);
                for (auto block = block_range->begin(); block != block_range->end(); ++block) {
                    auto end_idx = block.get_global_index();
                    for (int i = 0; i < N; i++) {
                        end_idx[i] += stride * interp_dim_limit;
                        if (end_idx[i] > global_dimensions[i] - 1) {
                            end_idx[i] = global_dimensions[i] - 1;
                        }
                    }
                    block_interpolation(dec_data, block.get_global_index(), end_idx, PB_recover,
                                        interpolators[interpolator_id], direction_sequence_id, stride, true);
                }
                quantizer.postdecompress_data();
                std::cout << "Level = " << level << " , quant size = " << quant_inds.size() << ", Time = " << timer.stop() << std::endl;
                quant_inds_count += quant_index;
            }


            for (uint level = level_fill; level > 1; level--) {
                Timer timer(true);
                size_t stride = 1U << (level - 1);
                block_interpolation(dec_data, global_begin, global_end, PB_fill,
                                    interpolators[interpolator_id], direction_sequence_id, stride, true);
                std::cout << "Fill Level = " << level << " " << std::endl;
            }
//            if (level_fill == 1) {

            std::vector<int> quant_sign;
//                size_t bsize = bitplane.size();
            int bitstart = 0;
            size_t quant_size;

            if (level_independent == 0) {
                block_interpolation(dec_data, global_begin, global_end, PB_fill,
                                    interpolators[interpolator_id], direction_sequence_id, 1, true);
                std::cout << "Fill Level = " << 1 << " " << std::endl;
            }


            std::fill(quant_inds.begin(), quant_inds.end(), 0);
            for (int b = 0; b < std::min<int>(bitplane.size(), level_independent); b++) {

                size_t length = lossless_size[lossless_id];
                retrieved_size += length;

                uchar *compressed_data = lossless.decompress(lossless_data, length);
                uchar const *compressed_data_pos = compressed_data;

                if (b == 0) {
                    quantizer.load(compressed_data_pos, length);
//                    eb = quantizer.get_eb();

                    read(quant_size, compressed_data_pos, length);
                    quant_inds.resize(quant_size);

                    encoder.load(compressed_data_pos, length);
                    quant_sign = encoder.decode(compressed_data_pos, quant_size);
                    encoder.postprocess_decode();

                    dec_delta.resize(num_elements, 0);
                }

                //                printf("%lu\n", quant_size);
                encoder.load(compressed_data_pos, length);
                auto quant_ind_truncated = encoder.decode(compressed_data_pos, quant_size);
                encoder.postprocess_decode();
                quantizer.reset_unpred_index();

                quant_index = 0;

                lossless.postdecompress_data(compressed_data);
                lossless_data += lossless_size[lossless_id];
                lossless_id++;

                for (size_t i = 0; i < quant_size; i++) {
                    if (quant_sign[i] == 0) { //unpredictable data
                        quant_inds[i] = -quantizer.get_radius();
                    } else {
                        quant_inds[i] = quant_sign[i] * recover_bits_to_uint(quant_ind_truncated[i], bitstart, bitplane[b]);
                    }
                }

                block_interpolation(dec_data, global_begin, global_end, (b == 0 ? PB_recover : PB_recover_delta),
                                    interpolators[interpolator_id], direction_sequence_id, 1, true);

                bitstart += bitplane[b];
                printf("Bitplane %d\n", b);
            }

            printf("retrieved = %.3f%% %lu\n", retrieved_size * 100.0 / (num_elements * sizeof(float)), retrieved_size);
            return dec_data;
        }

        // compress given the error bound
        uchar *compress(T *data, std::vector<size_t> &lossless_size) {
            Timer timer(true);

            quant_inds.reserve(num_elements);
//            quant_inds.resize(num_elements);
            size_t interp_compressed_size = 0;
//            debug.resize(num_elements, 0);
//            preds.resize(num_elements, 0);
            size_t quant_inds_total = 0;

            T eb = quantizer.get_eb();
            std::cout << "Absolute error bound = " << eb << std::endl;
//            quantizer.set_eb(eb * eb_ratio);

            uchar *lossless_data = new uchar[size_t((num_elements < 1000000 ? 4 : 1.2) * num_elements) * sizeof(T)];
            uchar *lossless_data_pos = lossless_data;

            write(global_dimensions.data(), N, lossless_data_pos);
            write(interp_dim_limit, lossless_data_pos);
            lossless_size.push_back(lossless_data_pos - lossless_data);

            for (uint level = levels; level > 1; level--) {
                timer.start();

                quantizer.set_eb((level >= 3) ? eb * eb_ratio : eb);
                uint stride = 1U << (level - 1);
//                std::cout << "Level = " << level << ", stride = " << stride << std::endl;

                if (level == levels) {
                    quant_inds.push_back(quantizer.quantize_and_overwrite(*data, 0));
                }
                auto block_range = std::make_shared<
                        SZ::multi_dimensional_range<T, N>>(data, std::begin(global_dimensions),
                                                           std::end(global_dimensions),
                                                           interp_dim_limit * stride, 0);

                for (auto block = block_range->begin(); block != block_range->end(); ++block) {
                    auto end_idx = block.get_global_index();
                    for (int i = 0; i < N; i++) {
                        end_idx[i] += interp_dim_limit * stride;
                        if (end_idx[i] > global_dimensions[i] - 1) {
                            end_idx[i] = global_dimensions[i] - 1;
                        }
                    }

                    block_interpolation(data, block.get_global_index(), end_idx, PB_predict_overwrite,
                                        interpolators[interpolator_id], direction_sequence_id, stride, true);

                }

                auto quant_size = quant_inds.size();
                quant_inds_total += quant_size;
                encode_lossless(lossless_data_pos, lossless_size);

                printf("level = %d , quant size = %lu , time=%.3f\n", level, quant_size, timer.stop());

            }

            timer.start();
            quantizer.set_eb(eb);
            block_interpolation(data, global_begin, global_end, PB_predict_overwrite,
                                interpolators[interpolator_id], direction_sequence_id, 1, true);
            printf("level = %d , quant size = %lu , prediction time=%.3f\n", 1, quant_inds.size(), timer.stop());
            quant_inds_total += quant_inds.size();


            Timer timer2(true);
            timer.start();
            int radius = quantizer.get_radius();
            size_t bsize = bitplane.size();
            size_t qsize = quant_inds.size();
            std::vector<int> quants(qsize);
            std::vector<int> quant_sign(qsize);
            uchar *compressed_data = new uchar[size_t((quant_inds.size() < 1000000 ? 10 : 1.2) * quant_inds.size()) * sizeof(T)];

            for (int b = 0, bitstart = 0; b < bsize; b++) {
                timer.start();
                uint32_t bitshifts = 32 - bitstart - bitplane[b];
                uint32_t bitmasks = (1 << bitplane[b]) - 1;
                bitstart += bitplane[b];
                uchar *compressed_data_pos = compressed_data;

                if (b == 0) {
                    quantizer.save(compressed_data_pos);
                    quantizer.clear();

                    write((size_t) qsize, compressed_data_pos);
                    for (size_t i = 0; i < qsize; i++) {
                        if (quant_inds[i] == -radius) {
                            quant_sign[i] = 0;
                            quant_inds[i] = 0;
                        } else if (quant_inds[i] < 0) {
                            quant_inds[i] = -quant_inds[i];
                            quant_sign[i] = -1;
                        } else {
                            quant_sign[i] = 1;
                        }
                    }

                    encoder.preprocess_encode(quant_sign, 0);
                    encoder.save(compressed_data_pos);
                    encoder.encode(quant_sign, compressed_data_pos);
                    encoder.postprocess_encode();

                }
                ska::unordered_map<int, size_t> frequency;
                for (size_t i = 0; i < qsize; i++) {
                    quants[i] = ((uint32_t) quant_inds[i]) >> bitshifts & bitmasks;
                    frequency[quants[i]]++;
                }
                encoder.preprocess_encode(quants, frequency);
                encoder.save(compressed_data_pos);
                encoder.encode(quants, compressed_data_pos);
                encoder.postprocess_encode();

                size_t size = 0;
                uchar *lossless_result = lossless.compress(
                        compressed_data, compressed_data_pos - compressed_data, size);
                memcpy(lossless_data_pos, lossless_result, size);
                lossless_data_pos += size;
                lossless_size.push_back(size);
                delete[]lossless_result;
                timer.stop("bitplane encode+lossless");

            }
            delete[]compressed_data;

//            quant_inds.clear();
            std::cout << "total element = " << num_elements << ", quantization element = " << quant_inds_total << std::endl;
            assert(quant_inds_total >= num_elements);

            timer2.stop("multilevel_quantization");
            return lossless_data;
        }

    private:

        void print_global_index(size_t offset) {
            std::array<size_t, N> global_idx{0};
            for (int i = N - 1; i >= 0; i--) {
                global_idx[i] = offset % global_dimensions[i];
                offset /= global_dimensions[i];
            }
            for (const auto &id: global_idx) {
                printf("%lu ", id);
            }
        }

        void lossless_decode(uchar const *&lossless_data_pos, const std::vector<size_t> &lossless_size, int lossless_id) {

            size_t remaining_length = lossless_size[lossless_id];
            retrieved_size += remaining_length;

            uchar *compressed_data = lossless.decompress(lossless_data_pos, remaining_length);
            uchar const *compressed_data_pos = compressed_data;

            quantizer.load(compressed_data_pos, remaining_length);
            double eb = quantizer.get_eb();

            size_t quant_size;
            read(quant_size, compressed_data_pos, remaining_length);
            //                printf("%lu\n", quant_size);
            if (quant_size < 128) {
                quant_inds.resize(quant_size);
                read(quant_inds.data(), quant_size, compressed_data_pos, remaining_length);
            } else {
                encoder.load(compressed_data_pos, remaining_length);
                quant_inds = encoder.decode(compressed_data_pos, quant_size);
                encoder.postprocess_decode();
            }
            quant_index = 0;

            lossless.postdecompress_data(compressed_data);
            lossless_data_pos += lossless_size[lossless_id];
        }

        void encode_lossless(uchar *&lossless_data_pos, std::vector<size_t> &lossless_size) {
            uchar *compressed_data = new uchar[size_t((quant_inds.size() < 1000000 ? 10 : 1.2) * quant_inds.size()) * sizeof(T)];
            uchar *compressed_data_pos = compressed_data;

            quantizer.save(compressed_data_pos);
            quantizer.postcompress_data();
            quantizer.clear();
            write((size_t) quant_inds.size(), compressed_data_pos);
            if (quant_inds.size() < 128) {
                write(quant_inds.data(), quant_inds.size(), compressed_data_pos);
            } else {
                encoder.preprocess_encode(quant_inds, 0);
                encoder.save(compressed_data_pos);
                encoder.encode(quant_inds, compressed_data_pos);
                encoder.postprocess_encode();
            }

            size_t size = 0;
            uchar *lossless_data_cur_level = lossless.compress(compressed_data,
                                                               compressed_data_pos - compressed_data,
                                                               size);
            lossless.postcompress_data(compressed_data);
            memcpy(lossless_data_pos, lossless_data_cur_level, size);
            lossless_data_pos += size;
            lossless_size.push_back(size);
            delete[]lossless_data_cur_level;

            quant_inds.clear();

        }

        enum PredictorBehavior {
            PB_predict_overwrite, PB_predict, PB_recover, PB_fill, PB_recover_delta
        };

        inline void quantize1(size_t idx, T &d, T pred) {
            auto quant = quantizer.quantize_and_overwrite(d, pred);
//            quant_inds.push_back(quant);
            quant_inds[idx] = quant;
        }

        inline void quantize(size_t idx, T &d, T pred) {
            quant_inds.push_back(quantizer.quantize_and_overwrite(d, pred));
        }

        inline void recover(size_t idx, T &d, T pred) {
            d = quantizer.recover(pred, quant_inds[quant_index++]);
        };


        inline void recover_delta(size_t idx, T &d, T pred) {
            quantizer.recover_delta(d, dec_delta[idx], pred, quant_inds[quant_index++]);
//            if (check_dec) {
//                if (fabs(d - ori_data[idx]) > 1.1e-3) {
//                    printf("ERROR %lu %lu %.5f %.5f ", quant_index-1, idx, ori_data[idx], d);
//                    print_global_index(idx);
//                    printf("\n");
//                }
//            }
        };

        inline void fill(size_t idx, T &d, T pred) {
            d = pred;
        }

        double block_interpolation_1d(T *data, size_t begin, size_t end, size_t stride,
                                      const std::string &interp_func,
                                      const PredictorBehavior pb) {
            size_t n = (end - begin) / stride + 1;
            if (n <= 1) {
                return 0;
            }
            double predict_error = 0;
            size_t stride3x = 3 * stride;
            size_t stride5x = 5 * stride;
//            printf("stride %lu %lu %lu\n", stride, stride3x, stride5x);
            if (interp_func == "linear" || n < 5) {
                if (pb == PB_predict_overwrite) {
                    for (size_t i = 1; i + 1 < n; i += 2) {
                        T *d = data + begin + i * stride;
                        quantize(d - data, *d, interp_linear(*(d - stride), *(d + stride)));
                    }
                    if (n % 2 == 0) {
                        T *d = data + begin + (n - 1) * stride;
                        if (n < 4) {
                            quantize(d - data, *d, *(d - stride));
                        } else {
                            quantize(d - data, *d, interp_linear1(*(d - stride3x), *(d - stride)));
                        }
                    }
                } else if (pb == PB_recover) {
                    for (size_t i = 1; i + 1 < n; i += 2) {
                        T *d = data + begin + i * stride;
                        recover(d - data, *d, interp_linear(*(d - stride), *(d + stride)));
                    }
                    if (n % 2 == 0) {
                        T *d = data + begin + (n - 1) * stride;
                        if (n < 4) {
                            recover(d - data, *d, *(d - stride));
                        } else {
                            recover(d - data, *d, interp_linear1(*(d - stride3x), *(d - stride)));
                        }
                    }
                } else if (pb == PB_recover_delta) {
                    for (size_t i = 1; i + 1 < n; i += 2) {
                        T *d = data + begin + i * stride;
                        recover_delta(d - data, *d, interp_linear(dec_delta[d - stride - data], dec_delta[d + stride - data]));
                    }
                    if (n % 2 == 0) {
                        T *d = data + begin + (n - 1) * stride;
                        if (n < 4) {
                            recover_delta(d - data, *d, dec_delta[d - stride - data]);
                        } else {
                            recover_delta(d - data, *d, interp_linear1(dec_delta[d - stride3x - data], dec_delta[d - stride - data]));
                        }
                    }
                } else {
                    for (size_t i = 1; i + 1 < n; i += 2) {
                        T *d = data + begin + i * stride;
                        fill(d - data, *d, interp_linear(*(d - stride), *(d + stride)));
//                        fill(d - data, *d, *(d - stride));
                    }
                    if (n % 2 == 0) {
                        T *d = data + begin + (n - 1) * stride;
                        if (n < 4) {
                            fill(d - data, *d, *(d - stride));
                        } else {
                            fill(d - data, *d, interp_linear1(*(d - stride3x), *(d - stride)));
//                            fill(d - data, *d, *(d - stride));
                        }
                    }
                }
            } else {
                if (pb == PB_predict_overwrite) {

                    T *d;
                    size_t i;

                    d = data + begin + stride;
                    quantize(d - data, *d, interp_quad_1(*(d - stride), *(d + stride), *(d + stride3x)));

                    for (i = 3; i + 3 < n; i += 2) {
                        d = data + begin + i * stride;
                        quantize(d - data, *d,
                                 interp_cubic(*(d - stride3x), *(d - stride), *(d + stride), *(d + stride3x)));
                    }

                    d = data + begin + i * stride;
                    quantize(d - data, *d, interp_quad_2(*(d - stride3x), *(d - stride), *(d + stride)));
                    if (n % 2 == 0) {
                        d = data + begin + (n - 1) * stride;
                        quantize(d - data, *d, interp_quad_3(*(d - stride5x), *(d - stride3x), *(d - stride)));
                    }

                } else if (pb == PB_recover) {
                    T *d;
                    size_t i;

                    d = data + begin + stride;
                    recover(d - data, *d, interp_quad_1(*(d - stride), *(d + stride), *(d + stride3x)));

                    for (i = 3; i + 3 < n; i += 2) {
                        d = data + begin + i * stride;
                        recover(d - data, *d, interp_cubic(*(d - stride3x), *(d - stride), *(d + stride), *(d + stride3x)));
                    }

                    d = data + begin + i * stride;
                    recover(d - data, *d, interp_quad_2(*(d - stride3x), *(d - stride), *(d + stride)));

                    if (n % 2 == 0) {
                        d = data + begin + (n - 1) * stride;
                        recover(d - data, *d, interp_quad_3(*(d - stride5x), *(d - stride3x), *(d - stride)));
                    }
                } else if (pb == PB_recover_delta) {
                    T *d;
                    size_t i;

                    d = data + begin + stride;
                    recover_delta(d - data, *d,
                                  interp_quad_1(dec_delta[d - stride - data], dec_delta[d + stride - data], dec_delta[d + stride3x - data]));

                    for (i = 3; i + 3 < n; i += 2) {
                        d = data + begin + i * stride;
                        recover_delta(d - data, *d,
                                      interp_cubic(dec_delta[d - stride3x - data], dec_delta[d - stride - data], dec_delta[d + stride - data],
                                                   dec_delta[d + stride3x - data]));
                    }

                    d = data + begin + i * stride;
                    recover_delta(d - data, *d,
                                  interp_quad_2(dec_delta[d - stride3x - data], dec_delta[d - stride - data], dec_delta[d + stride - data]));

                    if (n % 2 == 0) {
                        d = data + begin + (n - 1) * stride;
                        recover_delta(d - data, *d,
                                      interp_quad_3(dec_delta[d - stride5x - data], dec_delta[d - stride3x - data], dec_delta[d - stride - data]));
                    }
                } else {
                    T *d;
                    size_t i;

                    d = data + begin + stride;
                    fill(d - data, *d, interp_quad_1(*(d - stride), *(d + stride), *(d + stride3x)));

                    for (i = 3; i + 3 < n; i += 2) {
                        d = data + begin + i * stride;
                        fill(d - data, *d, interp_cubic(*(d - stride3x), *(d - stride), *(d + stride), *(d + stride3x)));
                    }

                    d = data + begin + i * stride;
                    fill(d - data, *d, interp_quad_2(*(d - stride3x), *(d - stride), *(d + stride)));

                    if (n % 2 == 0) {
                        d = data + begin + (n - 1) * stride;
                        fill(d - data, *d, interp_quad_3(*(d - stride5x), *(d - stride3x), *(d - stride)));
                    }
                }
            }

            return predict_error;
        }

        template<uint NN = N>
        typename std::enable_if<NN == 1, double>::type
        block_interpolation(T *data, std::array<size_t, N> begin, std::array<size_t, N> end, const PredictorBehavior pb,
                            const std::string &interp_func, const int direction, uint stride, bool overlap) {
            return block_interpolation_1d(data, begin[0], end[0], stride, interp_func, pb);
        }

        template<uint NN = N>
        typename std::enable_if<NN == 2, double>::type
        block_interpolation(T *data, std::array<size_t, N> begin, std::array<size_t, N> end, const PredictorBehavior pb,
                            const std::string &interp_func, const int direction, uint stride, bool overlap) {
            double predict_error = 0;
            size_t stride2x = stride * 2;
            std::array<int, N> dims = dimension_sequences[direction];
            for (size_t j = ((overlap && begin[dims[1]]) ? begin[dims[1]] + stride2x : begin[dims[1]]); j <= end[dims[1]]; j += stride2x) {
                size_t begin_offset = begin[dims[0]] * dimension_offsets[dims[0]] + j * dimension_offsets[dims[1]];
                predict_error += block_interpolation_1d(data, begin_offset,
                                                        begin_offset + (end[dims[0]] - begin[dims[0]]) * dimension_offsets[dims[0]],
                                                        stride * dimension_offsets[dims[0]], interp_func, pb);
            }
            for (size_t i = ((overlap && begin[dims[0]]) ? begin[dims[0]] + stride : begin[dims[0]]); i <= end[dims[0]]; i += stride) {
                size_t begin_offset = i * dimension_offsets[dims[0]] + begin[dims[1]] * dimension_offsets[dims[1]];
                predict_error += block_interpolation_1d(data, begin_offset,
                                                        begin_offset + (end[dims[1]] - begin[dims[1]]) * dimension_offsets[dims[1]],
                                                        stride * dimension_offsets[dims[1]], interp_func, pb);
            }
            return predict_error;
        }

        template<uint NN = N>
        typename std::enable_if<NN == 3, double>::type
        block_interpolation(T *data, std::array<size_t, N> begin, std::array<size_t, N> end, PredictorBehavior pb,
                            const std::string &interp_func, const int direction, uint stride, bool overlap) {
            double predict_error = 0;
            size_t stride2x = stride * 2;

            std::array<int, N> dims = dimension_sequences[direction];
            for (size_t j = ((overlap && begin[dims[1]]) ? begin[dims[1]] + stride2x : begin[dims[1]]); j <= end[dims[1]]; j += stride2x) {
                for (size_t k = ((overlap && begin[dims[2]]) ? begin[dims[2]] + stride2x : begin[dims[2]]); k <= end[dims[2]]; k += stride2x) {
                    size_t begin_offset = begin[dims[0]] * dimension_offsets[dims[0]] + j * dimension_offsets[dims[1]]
                                          + k * dimension_offsets[dims[2]];
                    predict_error += block_interpolation_1d(data, begin_offset,
                                                            begin_offset + (end[dims[0]] - begin[dims[0]]) * dimension_offsets[dims[0]],
                                                            stride * dimension_offsets[dims[0]], interp_func, pb);
                }
            }


            for (size_t i = ((overlap && begin[dims[0]]) ? begin[dims[0]] + stride : begin[dims[0]]); i <= end[dims[0]]; i += stride) {
                for (size_t k = ((overlap && begin[dims[2]]) ? begin[dims[2]] + stride2x : begin[dims[2]]); k <= end[dims[2]]; k += stride2x) {
                    size_t begin_offset = i * dimension_offsets[dims[0]] + begin[dims[1]] * dimension_offsets[dims[1]] +
                                          k * dimension_offsets[dims[2]];
                    predict_error += block_interpolation_1d(data, begin_offset,
                                                            begin_offset + (end[dims[1]] - begin[dims[1]]) * dimension_offsets[dims[1]],
                                                            stride * dimension_offsets[dims[1]], interp_func, pb);
                }
            }

            for (size_t i = ((overlap && begin[dims[0]]) ? begin[dims[0]] + stride : begin[dims[0]]); i <= end[dims[0]]; i += stride) {
                for (size_t j = ((overlap && begin[dims[1]]) ? begin[dims[1]] + stride : begin[dims[1]]); j <= end[dims[1]]; j += stride) {
                    size_t begin_offset = i * dimension_offsets[dims[0]] + j * dimension_offsets[dims[1]] +
                                          begin[dims[2]] * dimension_offsets[dims[2]];
                    predict_error += block_interpolation_1d(data, begin_offset,
                                                            begin_offset + (end[dims[2]] - begin[dims[2]]) * dimension_offsets[dims[2]],
                                                            stride * dimension_offsets[dims[2]], interp_func, pb);
                }
            }


            return predict_error;
        }


        template<uint NN = N>
        typename std::enable_if<NN == 4, double>::type
        block_interpolation(T *data, std::array<size_t, N> begin, std::array<size_t, N> end, const PredictorBehavior pb,
                            const std::string &interp_func, const int direction, uint stride, bool overlap) {
            double predict_error = 0;
            size_t stride2x = stride * 2;
            max_error = 0;
            std::array<int, N> dims = dimension_sequences[direction];
            for (size_t j = (begin[dims[1]] ? begin[dims[1]] + stride2x : 0); j <= end[dims[1]]; j += stride2x) {
                for (size_t k = (begin[dims[2]] ? begin[dims[2]] + stride2x : 0); k <= end[dims[2]]; k += stride2x) {
                    for (size_t t = (begin[dims[3]] ? begin[dims[3]] + stride2x : 0);
                         t <= end[dims[3]]; t += stride2x) {
                        size_t begin_offset =
                                begin[dims[0]] * dimension_offsets[dims[0]] + j * dimension_offsets[dims[1]] +
                                k * dimension_offsets[dims[2]] +
                                t * dimension_offsets[dims[3]];
                        predict_error += block_interpolation_1d(data, begin_offset,
                                                                begin_offset +
                                                                (end[dims[0]] - begin[dims[0]]) *
                                                                dimension_offsets[dims[0]],
                                                                stride * dimension_offsets[dims[0]], interp_func, pb);
                    }
                }
            }
//            printf("%.8f ", max_error);
            max_error = 0;
            for (size_t i = (begin[dims[0]] ? begin[dims[0]] + stride : 0); i <= end[dims[0]]; i += stride) {
                for (size_t k = (begin[dims[2]] ? begin[dims[2]] + stride2x : 0); k <= end[dims[2]]; k += stride2x) {
                    for (size_t t = (begin[dims[3]] ? begin[dims[3]] + stride2x : 0);
                         t <= end[dims[3]]; t += stride2x) {
                        size_t begin_offset =
                                i * dimension_offsets[dims[0]] + begin[dims[1]] * dimension_offsets[dims[1]] +
                                k * dimension_offsets[dims[2]] +
                                t * dimension_offsets[dims[3]];
                        predict_error += block_interpolation_1d(data, begin_offset,
                                                                begin_offset +
                                                                (end[dims[1]] - begin[dims[1]]) *
                                                                dimension_offsets[dims[1]],
                                                                stride * dimension_offsets[dims[1]], interp_func, pb);
                    }
                }
            }
//            printf("%.8f ", max_error);
            max_error = 0;
            for (size_t i = (begin[dims[0]] ? begin[dims[0]] + stride : 0); i <= end[dims[0]]; i += stride) {
                for (size_t j = (begin[dims[1]] ? begin[dims[1]] + stride : 0); j <= end[dims[1]]; j += stride) {
                    for (size_t t = (begin[dims[3]] ? begin[dims[3]] + stride2x : 0);
                         t <= end[dims[3]]; t += stride2x) {
                        size_t begin_offset = i * dimension_offsets[dims[0]] + j * dimension_offsets[dims[1]] +
                                              begin[dims[2]] * dimension_offsets[dims[2]] +
                                              t * dimension_offsets[dims[3]];
                        predict_error += block_interpolation_1d(data, begin_offset,
                                                                begin_offset +
                                                                (end[dims[2]] - begin[dims[2]]) *
                                                                dimension_offsets[dims[2]],
                                                                stride * dimension_offsets[dims[2]], interp_func, pb);
                    }
                }
            }

//            printf("%.8f ", max_error);
            max_error = 0;
            for (size_t i = (begin[dims[0]] ? begin[dims[0]] + stride : 0); i <= end[dims[0]]; i += stride) {
                for (size_t j = (begin[dims[1]] ? begin[dims[1]] + stride : 0); j <= end[dims[1]]; j += stride) {
                    for (size_t k = (begin[dims[2]] ? begin[dims[2]] + stride : 0); k <= end[dims[2]]; k += stride) {
                        size_t begin_offset =
                                i * dimension_offsets[dims[0]] + j * dimension_offsets[dims[1]] +
                                k * dimension_offsets[dims[2]] +
                                begin[dims[3]] * dimension_offsets[dims[3]];
                        predict_error += block_interpolation_1d(data, begin_offset,
                                                                begin_offset +
                                                                (end[dims[3]] - begin[dims[3]]) *
                                                                dimension_offsets[dims[3]],
                                                                stride * dimension_offsets[dims[3]], interp_func, pb);
                    }
                }
            }
//            printf("%.8f \n", max_error);
            return predict_error;
        }

        int levels = -1;
        int level_independent = -1;
        int level_fill = 0;
        int interpolator_id;
        size_t interp_dim_limit, block_size;
        double eb_ratio = 0.5;
        std::vector<std::string> interpolators;
        std::vector<int> quant_inds;
        size_t quant_index = 0; // for decompress
        size_t num_elements;
        std::array<size_t, N> global_dimensions, global_begin, global_end;
        std::array<size_t, N> dimension_offsets;
        std::vector<std::array<int, N>> dimension_sequences;
        int direction_sequence_id;
        Quantizer quantizer;
        Encoder encoder;
        Lossless lossless;

        std::vector<int> bitplane = {24, 4, 2, 2};
        std::vector<T> dec_delta;
        size_t retrieved_size = 0;

        //debug only
//        bool check_dec = false;
        double max_error;
//        float eb;
//        std::vector<T> ori_data;
//        std::vector<int> debug;
//        std::vector<size_t> debug_idx_list;
//        std::vector<int> debug;
//        std::vector<T> preds;

    };


};


#endif

