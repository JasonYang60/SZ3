#ifndef SZ3_SZ_LORENZO_REG_HPP
#define SZ3_SZ_LORENZO_REG_HPP

#include "SZ3/compressor/SZBlockCompressor.hpp"
#include "SZ3/quantizer/IntegerQuantizer.hpp"
#include "SZ3/predictor/LorenzoPredictor.hpp"
#include "SZ3/predictor/RegressionPredictor.hpp"
#include "SZ3/predictor/ComposedPredictor.hpp"
#include "SZ3/encoder/HuffmanEncoder.hpp"
#include "SZ3/lossless/Lossless_zstd.hpp"
#include "SZ3/utils/Statistic.hpp"
#include "SZ3/utils/Extraction.hpp"
#include "SZ3/utils/QuantOptimizatioin.hpp"
#include "SZ3/utils/Config.hpp"
#include "SZ3/def.hpp"
#include <cmath>
#include <memory>


template<class T, SZ::uint N, class Quantizer, class Encoder, class Lossless>
std::shared_ptr<SZ::concepts::CompressorInterface<T>>
make_lorenzo_regression_compressor(const SZ::Config &conf, Quantizer quantizer, Encoder encoder, Lossless lossless) {
    std::vector<std::shared_ptr<SZ::concepts::PredictorInterface<T, N>>> predictors;

    int methodCnt = (conf.lorenzo + conf.lorenzo2 + conf.regression);
    int use_single_predictor = (methodCnt == 1);
    if (methodCnt == 0) {
        throw std::invalid_argument("All lorenzo and regression methods are disabled");
    }

    if (conf.lorenzo) {
        if (use_single_predictor) {
            return SZ::make_sz_block_compressor<T, N>(conf, SZ::LorenzoPredictor<T, N, 1, Quantizer>(quantizer, conf.absErrorBound),
                                                        encoder, lossless);
        } else {
            predictors.push_back(std::make_shared<SZ::LorenzoPredictor<T, N, 1, Quantizer>>(quantizer, conf.absErrorBound));
        }
    }
    if (conf.lorenzo2) {
        if (use_single_predictor) {
            return SZ::make_sz_block_compressor<T, N>(conf, SZ::LorenzoPredictor<T, N, 2, Quantizer>(quantizer, conf.absErrorBound),
                                                        encoder, lossless);
        } else {
            predictors.push_back(std::make_shared<SZ::LorenzoPredictor<T, N, 2, Quantizer>>(quantizer, conf.absErrorBound));
        }
    }
    if (conf.regression) {
        if (use_single_predictor) {
            return SZ::make_sz_block_compressor<T, N>(conf, SZ::RegressionPredictor<T, N, Quantizer>(quantizer, conf.blockSize, conf.absErrorBound),
                                                        encoder, lossless);
        } else {
            predictors.push_back(std::make_shared<SZ::RegressionPredictor<T, N, Quantizer>>(quantizer, conf.blockSize, conf.absErrorBound));
        }
    }

    return SZ::make_sz_block_compressor<T, N>(conf, SZ::ComposedPredictor<T, N>(predictors), encoder, lossless);
}


template<class T, SZ::uint N>
char *SZ_compress_LorenzoReg(SZ::Config &conf, T *data, size_t &outSize) {

    assert(N == conf.N);
    assert(conf.cmprAlgo == SZ::ALGO_LORENZO_REG);
    SZ::calAbsErrorBound(conf, data);

    auto sz = make_lorenzo_regression_compressor<T, N>(conf, SZ::LinearQuantizer<T>(conf.absErrorBound, conf.quantbinCnt / 2),
                                                       SZ::HuffmanEncoder<int>(), SZ::Lossless_zstd());

    return (char *) sz->compress(conf, data, outSize);
}


template<class T, SZ::uint N>
void SZ_decompress_LorenzoReg(const SZ::Config &conf, char *cmpData, size_t cmpSize, T *decData) {
    assert(conf.cmprAlgo == SZ::ALGO_LORENZO_REG);

    SZ::uchar const *cmpDataPos = (SZ::uchar *) cmpData;
    auto sz = make_lorenzo_regression_compressor<T, N>(conf, SZ::LinearQuantizer<T>(), SZ::HuffmanEncoder<int>(), SZ::Lossless_zstd());
    sz->decompress(cmpDataPos, cmpSize, decData);

}

#endif