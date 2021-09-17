#ifndef SZ_Truncate_COMPRESSOR_HPP
#define SZ_Truncate_COMPRESSOR_HPP

#include "compressor/Compressor.hpp"
#include "frontend/Frontend.hpp"
#include "encoder/Encoder.hpp"
#include "lossless/Lossless.hpp"
#include "utils/FileUtil.hpp"
#include "utils/Config.hpp"
#include "utils/Timer.hpp"
#include "utils/ByteUtil.hpp"
#include "def.hpp"
#include <cstring>

namespace SZ {
    template<class T, uint N, class Lossless>
    class SZTruncateCompressor : public concepts::CompressorInterface<T> {
    public:


        SZTruncateCompressor(const Config<T, N> &conf, Lossless lossless, int byteLens) :
                lossless(lossless), conf(conf), byteLen(byteLens) {
            static_assert(std::is_base_of<concepts::LosslessInterface, Lossless>::value,
                          "must implement the lossless interface");
        }

        uchar *compress(T *data, size_t &compressed_size) {

            auto compressed_data = new uchar[conf.num * sizeof(T)];
            auto compressed_data_pos = (uchar *) compressed_data;

            Timer timer(true);
            truncateArray(data, conf.num, byteLen, compressed_data_pos);
            timer.stop("Prediction & Quantization");

            uchar *lossless_data = lossless.compress(compressed_data,
                                                     (uchar *) compressed_data_pos - compressed_data,
                                                     compressed_size);
            lossless.postcompress_data(compressed_data);
            return lossless_data;
        }


        T *decompress(uchar const *lossless_compressed_data, const size_t length) {
            size_t remaining_length = length;

            auto compressed_data = lossless.decompress(lossless_compressed_data, remaining_length);
            auto compressed_data_pos = (uchar *) compressed_data;

            Timer timer(true);
            auto dec_data = new T[conf.num];
            truncateArrayRecover(compressed_data_pos, conf.num, byteLen, dec_data);

            lossless.postdecompress_data(compressed_data);
            timer.stop("Prediction & Recover");
            return dec_data;
        }


    private:
        Lossless lossless;
        Config<T, N> conf;
        int byteLen = 2;
    };

    template<class T, uint N, class Lossless>
    SZTruncateCompressor<T, N, Lossless>
    make_sz_truncate_compressor(const Config<T, N> &conf, Lossless lossless, int byteLens) {
        return SZTruncateCompressor<T, N, Lossless>(conf, lossless, byteLens);
    }
}
#endif
