#pragma once

#include <eacp/GPU/Codegen/ComputeProgram.h>
#include <eacp/GPU/Device/Device.h>
#include <eacp/ML/Loader/SafetensorsFile.h>
#include <eacp/ML/Tensor/Tensor.h>

#include <string>
#include <vector>

namespace eacp::SA3Codec
{
std::vector<float> computeWeightNormFlat(const float* gain,
                                         const float* direction,
                                         int outChannels,
                                         int elementsPerOutput);

struct WNConv1dWeights
{
    ML::Tensor flatWeight;
    ML::Tensor bias;
    bool hasBias = false;
    int inChannels = 0;
    int outChannels = 0;
    int kernelSize = 0;
};

WNConv1dWeights loadWNConv1d(const ML::SafetensorsFile& file,
                             const std::string& prefix,
                             int inChannels,
                             int outChannels,
                             int kernelSize,
                             bool hasBias,
                             GPU::Device& device = GPU::Device::shared());

ML::Tensor applyWNConv1d(GPU::ComputePass& pass,
                         const ML::Tensor& input,
                         const WNConv1dWeights& weights,
                         GPU::Device& device = GPU::Device::shared());
}
