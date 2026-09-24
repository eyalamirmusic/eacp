#pragma once

#include <eacp/GPU/Device/Device.h>
#include <eacp/ML/Loader/SafetensorsFile.h>
#include <eacp/ML/Tensor/Tensor.h>

#include <vector>

namespace eacp::SA3DiT
{
constexpr auto embedDim = 1024;
constexpr auto depth = 20;
constexpr auto numHeads = 16;
constexpr auto headDim = 64;
constexpr auto condTokenDim = 768;
constexpr auto ioChannels = 256;
constexpr auto numMemoryTokens = 64;
constexpr auto localAddCondDim = 257;
constexpr auto timestepFeaturesDim = 256;
constexpr auto timestepMinFreq = 0.5f;
constexpr auto timestepMaxFreq = 10000.0f;
constexpr auto secondsMinVal = 0.0f;
constexpr auto secondsMaxVal = 384.0f;
constexpr auto rmsNormEpsilon = 1e-5f;
constexpr auto qkNormEpsilon = 1e-6f;

struct DiTConfig
{
    int embedDim;
    int depth;
    int numHeads;
    int headDim;
    int condTokenDim;
    int ioChannels;
    int numMemoryTokens;
    int localAddCondDim;
    int timestepFeaturesDim;
    float timestepMinFreq;
    float timestepMaxFreq;
    float secondsMinVal;
    float secondsMaxVal;
    float rmsNormEpsilon;
    float qkNormEpsilon;
    bool differential;

    static DiTConfig smallMusic();
    static DiTConfig medium();
};

struct LayerWeights
{
    ML::Tensor preNormGamma;
    ML::Tensor selfAttnQNormGamma;
    ML::Tensor selfAttnKNormGamma;
    ML::Tensor selfAttnQKVWeight;
    ML::Tensor selfAttnOutWeight;
    ML::Tensor crossAttendNormGamma;
    ML::Tensor crossAttnQNormGamma;
    ML::Tensor crossAttnKNormGamma;
    ML::Tensor crossAttnQWeight;
    ML::Tensor crossAttnKVWeight;
    ML::Tensor crossAttnOutWeight;
    ML::Tensor toLocalEmbed0Weight;
    ML::Tensor toLocalEmbed0Bias;
    ML::Tensor toLocalEmbed2Weight;
    ML::Tensor toLocalEmbed2Bias;
    ML::Tensor ffNormGamma;
    ML::Tensor ff0ProjWeight;
    ML::Tensor ff0ProjBias;
    ML::Tensor ff2Weight;
    ML::Tensor ff2Bias;
    ML::Tensor toScaleShiftGate;
};

struct Weights
{
    DiTConfig config;
    ML::Tensor preprocessConvWeight;
    ML::Tensor postprocessConvWeight;
    ML::Tensor toCondEmbed0Weight;
    ML::Tensor toCondEmbed2Weight;
    ML::Tensor toGlobalEmbed0Weight;
    ML::Tensor toGlobalEmbed2Weight;
    ML::Tensor toTimestepEmbed0Weight;
    ML::Tensor toTimestepEmbed0Bias;
    ML::Tensor toTimestepEmbed2Weight;
    ML::Tensor toTimestepEmbed2Bias;
    ML::Tensor secondsEmbedWeight;
    ML::Tensor secondsEmbedBias;
    ML::Tensor memoryTokens;
    ML::Tensor projectInWeight;
    ML::Tensor projectOutWeight;
    ML::Tensor rotaryInvFreq;
    ML::Tensor globalCondEmbedder0Weight;
    ML::Tensor globalCondEmbedder0Bias;
    ML::Tensor globalCondEmbedder2Weight;
    ML::Tensor globalCondEmbedder2Bias;
    std::vector<LayerWeights> layers;
};

Weights loadWeights(const ML::SafetensorsFile& file,
                    const DiTConfig& config = DiTConfig::smallMusic(),
                    GPU::Device& device = GPU::Device::shared());
}
