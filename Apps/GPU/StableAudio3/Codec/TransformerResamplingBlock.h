#pragma once

#include "CodecTransformerBlock.h"
#include "WNConv1d.h"

#include <eacp/GPU/Codegen/ComputeProgram.h>
#include <eacp/GPU/Device/Device.h>
#include <eacp/ML/Tensor/Tensor.h>

#include <vector>

namespace eacp::SA3Codec
{
struct ResamplingBlockWeights
{
    bool isEncoder = true;
    int inChannels = 0;
    int outChannels = 0;
    int stride = 0;
    int chunkSize = 0;
    int transformerDepth = 0;
    WNConv1dWeights mapping;
    ML::Tensor newTokens;
    std::vector<CodecBlockWeights> layers;
};

ML::Tensor foldWithNewTokens(GPU::ComputePass& pass,
                             const ML::Tensor& input,
                             int inputSegSize,
                             int outputSegSize,
                             const ML::Tensor& newTokens,
                             GPU::Device& device = GPU::Device::shared());

ML::Tensor unfoldLastSegment(GPU::ComputePass& pass,
                             const ML::Tensor& input,
                             int subChunkSize,
                             int outputSegSize,
                             GPU::Device& device = GPU::Device::shared());

ML::Tensor applyTransformerResamplingBlock(GPU::ComputePass& pass,
                                          const ML::Tensor& input,
                                          const ResamplingBlockWeights& weights,
                                          GPU::Device& device = GPU::Device::shared());
}
