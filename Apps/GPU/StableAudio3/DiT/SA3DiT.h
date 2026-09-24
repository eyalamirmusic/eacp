#pragma once

#include "Ops.h"
#include "Weights.h"

#include <eacp/GPU/Codegen/KernelCache.h>
#include <eacp/GPU/Device/Device.h>
#include <eacp/GPU/Frame/ComputePass.h>
#include <eacp/ML/Tensor/Tensor.h>

#include <optional>
#include <vector>

namespace eacp::SA3DiT
{
// What one layer's cross-attention reads from the prompt: its normed keys (and
// the differential second set) and its values. They depend on the prompt and
// the weights and on nothing a step changes, so they are made once per prompt.
struct PromptKeys
{
    ML::Tensor keys;
    std::optional<ML::Tensor> diffKeys;
    ML::Tensor keysAndValues;
    int valueColumn = 0;

    ML::TensorView values() const
    {
        return keysAndValues.columns(valueColumn, keys.cols());
    }
};

struct Prompt
{
    std::vector<PromptKeys> layers;
};

// The prompt's keys and values for every layer, from the text encoder's
// embeddings. The second form records and submits a command buffer of its own.
Prompt preparePrompt(GPU::ComputePass& pass,
                     const Weights& weights,
                     const ML::Tensor& crossAttnContext,
                     GPU::Device& device = GPU::Device::shared());

Prompt preparePrompt(const Weights& weights,
                     const ML::Tensor& crossAttnContext,
                     GPU::Device& device = GPU::Device::shared());

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
                            const DiTConfig& config,
                            const LayerWeights& layer,
                            const ML::Tensor& x,
                            const ML::Tensor& rotaryInvFreq,
                            const ML::Tensor& globalCondBase,
                            const ML::Tensor& crossAttnContext,
                            bool applyLocalConditioning = false,
                            GPU::Device& device = GPU::Device::shared());

ML::Tensor transformerBlock(GPU::ComputePass& pass,
                            const DiTConfig& config,
                            const LayerWeights& layer,
                            const ML::Tensor& x,
                            const ML::Tensor& rotaryInvFreq,
                            const ML::Tensor& globalCondBase,
                            const PromptKeys& prompt,
                            bool applyLocalConditioning = false,
                            GPU::Device& device = GPU::Device::shared());

ML::Tensor forward(GPU::ComputePass& pass,
                   const Weights& weights,
                   const ML::Tensor& latent,
                   float timestep,
                   float secondsTotal,
                   const Prompt& prompt,
                   GPU::Device& device = GPU::Device::shared());

// forward() with the prompt prepared in the same pass: one step's worth of
// work more than the form above, for a caller that runs one step.
ML::Tensor forward(GPU::ComputePass& pass,
                   const Weights& weights,
                   const ML::Tensor& latent,
                   float timestep,
                   float secondsTotal,
                   const ML::Tensor& crossAttnContext,
                   GPU::Device& device = GPU::Device::shared());

// Every kernel a forward pass dispatches, for a caller to build ahead of the
// first step.
}
