#pragma once

#include "Ops.h"
#include "Weights.h"

#include <eacp/GPU/Device/Device.h>
#include <eacp/GPU/Frame/ComputePass.h>
#include <eacp/ML/Tensor/Tensor.h>

namespace eacp::SA3DiT
{
ML::Tensor timestepEmbedding(GPU::ComputePass& pass,
                             const Weights& weights,
                             float timestep,
                             GPU::Device& device = GPU::Device::shared());

ML::Tensor globalConditioning(GPU::ComputePass& pass,
                              const Weights& weights,
                              float timestep,
                              float secondsTotal,
                              GPU::Device& device = GPU::Device::shared());

ML::Tensor transformerBlock(GPU::ComputePass& pass,
                            const LayerWeights& layer,
                            const ML::Tensor& x,
                            const ML::Tensor& rotaryInvFreq,
                            const ML::Tensor& globalCondBase,
                            const ML::Tensor& crossAttnContext,
                            bool applyLocalConditioning = false,
                            GPU::Device& device = GPU::Device::shared());

ML::Tensor forward(GPU::ComputePass& pass,
                   const Weights& weights,
                   const ML::Tensor& latent,
                   float timestep,
                   float secondsTotal,
                   const ML::Tensor& crossAttnContext,
                   GPU::Device& device = GPU::Device::shared());
}
