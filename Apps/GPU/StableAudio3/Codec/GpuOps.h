#pragma once

#include <eacp/GPU/Codegen/ComputeProgram.h>
#include <eacp/GPU/Codegen/KernelCache.h>
#include <eacp/GPU/Device/Device.h>
#include <eacp/ML/Tensor/Tensor.h>

#include <vector>

namespace eacp::SA3Codec
{
ML::Tensor foldWithNewTokensGpu(GPU::ComputePass& pass,
                                const ML::Tensor& input,
                                int inputSegSize,
                                int outputSegSize,
                                const ML::Tensor& newTokens,
                                GPU::Device& device = GPU::Device::shared());

ML::Tensor unfoldLastSegmentGpu(GPU::ComputePass& pass,
                                const ML::Tensor& input,
                                int subChunkSize,
                                int outputSegSize,
                                GPU::Device& device = GPU::Device::shared());

ML::Tensor conv1dUnfoldGpu(GPU::ComputePass& pass,
                           const ML::Tensor& input,
                           int inChannels,
                           int kernelSize,
                           GPU::Device& device = GPU::Device::shared());
}
