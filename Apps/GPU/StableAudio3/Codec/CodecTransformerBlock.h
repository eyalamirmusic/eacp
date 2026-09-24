#pragma once

#include <eacp/GPU/Codegen/ComputeProgram.h>
#include <eacp/GPU/Codegen/KernelCache.h>
#include <eacp/GPU/Device/Device.h>
#include <eacp/ML/Kernels/BandedAttention.h>
#include <eacp/ML/Tensor/Tensor.h>

namespace eacp::SA3Codec
{
struct DynamicTanhWeights
{
    ML::Tensor gamma;
    ML::Tensor beta;
    float alpha = 4.f;
};

struct CodecBlockWeights
{
    DynamicTanhWeights preNorm;
    ML::Tensor qkvWeight;
    ML::Tensor toOutWeight;
    DynamicTanhWeights qNorm;
    DynamicTanhWeights kNorm;
    ML::Tensor invFreq;
    DynamicTanhWeights ffNorm;
    ML::Tensor ff0Weight;
    ML::Tensor ff0Bias;
    ML::Tensor ff2Weight;
    ML::Tensor ff2Bias;
    int heads = 0;
    int headDim = 0;
    bool useSinusoidalGate = false;
};

ML::Tensor applyCodecTransformerBlock(GPU::ComputePass& pass,
                                      const ML::Tensor& input,
                                      const CodecBlockWeights& weights,
                                      const ML::AttentionBand& band,
                                      GPU::Device& device = GPU::Device::shared());

}
