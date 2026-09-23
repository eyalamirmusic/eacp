#pragma once

#include "../SA3DiT/SA3DiT.h"
#include "../SA3DiT/Weights.h"

#include <cstdint>
#include <functional>
#include <vector>

namespace eacp::SA3Sampler
{
using NoiseSource = std::function<std::vector<float>(int count)>;

using ModelForward =
    std::function<ML::Tensor(GPU::ComputePass&, const ML::Tensor&, float timestep)>;

NoiseSource randomNoiseSource(std::uint64_t seed);

ML::Tensor pingpongSampleWithModel(const ModelForward& model,
                                   int latentRows,
                                   int latentColumns,
                                   int steps,
                                   const NoiseSource& noiseSource,
                                   GPU::Device& device = GPU::Device::shared());

ML::Tensor pingpongSample(const SA3DiT::Weights& weights,
                          const ML::Tensor& crossAttnContext,
                          int latentLength,
                          float secondsTotal,
                          int steps,
                          const NoiseSource& noiseSource,
                          GPU::Device& device = GPU::Device::shared());
}
