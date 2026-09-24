#pragma once

#include <eacp/GPU/Codegen/KernelCache.h>
#include <eacp/GPU/Frame/ComputePass.h>
#include <eacp/ML/Loader/SafetensorsFile.h>
#include <eacp/ML/Tensor/Tensor.h>

#include <optional>
#include <string>
#include <vector>

namespace eacp::SA3TextEncoder
{
class T5GemmaEncoder
{
public:
    static constexpr int hiddenSize = 768;
    static constexpr int numLayers = 12;
    static constexpr int numHeads = 12;
    static constexpr int headDim = 64;

    static std::optional<T5GemmaEncoder> load(const std::string& safetensorsPath,
                                              GPU::Device& device = GPU::Device::shared());

    ML::Tensor encodeTokens(GPU::ComputePass& pass,
                            const std::vector<int>& tokenIds,
                            int validLength,
                            GPU::Device& device = GPU::Device::shared()) const;

    ML::Tensor encodeTokensThroughLayer(GPU::ComputePass& pass,
                                        const std::vector<int>& tokenIds,
                                        int validLength,
                                        int layerCount,
                                        GPU::Device& device = GPU::Device::shared()) const;

private:
    struct Layer
    {
        ML::Tensor preSelfAttnNormGamma;
        ML::Tensor postSelfAttnNormGamma;
        ML::Tensor preFeedforwardNormGamma;
        ML::Tensor postFeedforwardNormGamma;
        ML::Tensor qWeight;
        ML::Tensor kWeight;
        ML::Tensor vWeight;
        ML::Tensor oWeight;
        ML::Tensor gateWeight;
        ML::Tensor upWeight;
        ML::Tensor downWeight;
    };

    T5GemmaEncoder(ML::SafetensorsFile fileToUse,
                  std::vector<Layer> layersToUse,
                  ML::Tensor finalNormGammaToUse,
                  ML::Tensor invFreqToUse);

    ML::Tensor runLayers(GPU::ComputePass& pass,
                        ML::Tensor hidden,
                        const ML::Tensor& mask,
                        int layerCount,
                        GPU::Device& device) const;

    ML::SafetensorsFile file;
    std::vector<Layer> layers;
    ML::Tensor finalNormGamma;
    ML::Tensor invFreq;
};
}
