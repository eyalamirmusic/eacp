#pragma once

#include <eacp/GPU/Codegen/ComputeProgram.h>
#include <eacp/GPU/Codegen/KernelCache.h>
#include <eacp/GPU/Device/Device.h>
#include <eacp/ML/Tensor/Tensor.h>

namespace eacp::SA3Codec
{
struct SoftNormBottleneckWeights
{
    ML::Tensor scalingFactor;
    ML::Tensor bias;
    float runningStd = 1.f;
};

ML::Tensor softNormBottleneckEncode(GPU::ComputePass& pass,
                                    const ML::Tensor& input,
                                    const SoftNormBottleneckWeights& weights,
                                    GPU::Device& device = GPU::Device::shared());

ML::Tensor softNormBottleneckDecode(GPU::ComputePass& pass,
                                    const ML::Tensor& input,
                                    const SoftNormBottleneckWeights& weights,
                                    GPU::Device& device = GPU::Device::shared());

// Every kernel this file builds, handed over for the shader golden corpus.
void forEachBottleneckShaderGraph(const GPU::ShaderGraphVisitor& visit);
}
