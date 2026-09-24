#pragma once

#include "CodecConfig.h"
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
    CodecAttentionMode attentionMode = CodecAttentionMode::ChunkMidpointShift;
    int slidingWindowRadiusChunks = 1;
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

// The chunked stack attends within whole chunks and has no answer for a
// partial one: the reference folds the rows with einops, which refuses a
// length that is not a multiple, and pads upstream so it always is. This is
// that refusal, thrown as std::invalid_argument before anything is dispatched.
void checkChunkedRows(int rows, int effectiveChunkSize);

ML::Tensor
    applyTransformerResamplingBlock(const ML::Tensor& input,
                                    const ResamplingBlockWeights& weights,
                                    GPU::Device& device = GPU::Device::shared());
} // namespace eacp::SA3Codec
