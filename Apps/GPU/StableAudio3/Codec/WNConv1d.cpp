#include "WNConv1d.h"

#include "GpuOps.h"

#include <eacp/GPU/Frame/ComputePass.h>
#include <eacp/ML/Kernels/Linear.h>

#include <cmath>

namespace eacp::SA3Codec
{
using namespace eacp::GPU;
using namespace eacp::ML;

std::vector<float> computeWeightNormFlat(const float* gain,
                                         const float* direction,
                                         int outChannels,
                                         int elementsPerOutput)
{
    auto flat = std::vector<float>((std::size_t) outChannels * (std::size_t) elementsPerOutput);

    for (auto out = 0; out < outChannels; ++out)
    {
        auto base = (std::size_t) out * (std::size_t) elementsPerOutput;

        auto sumOfSquares = 0.0;
        for (auto i = std::size_t {}; i < (std::size_t) elementsPerOutput; ++i)
        {
            auto value = (double) direction[base + i];
            sumOfSquares += value * value;
        }

        auto norm = (float) std::sqrt(sumOfSquares);
        auto scale = gain[out] / norm;

        for (auto i = std::size_t {}; i < (std::size_t) elementsPerOutput; ++i)
            flat[base + i] = direction[base + i] * scale;
    }

    return flat;
}

WNConv1dWeights loadWNConv1d(const SafetensorsFile& file,
                             const std::string& prefix,
                             int inChannels,
                             int outChannels,
                             int kernelSize,
                             bool hasBias,
                             Device& device)
{
    auto gain = file.readF32(prefix + ".weight_g");
    auto direction = file.readF32(prefix + ".weight_v");

    auto elementsPerOutput = inChannels * kernelSize;
    auto flat = computeWeightNormFlat(
        gain.data(), direction.data(), outChannels, elementsPerOutput);

    auto flatWeightTensor =
        Tensor::fromHostF32(flat.data(), {outChannels, elementsPerOutput}, device);

    auto zeros = std::vector<float>((std::size_t) outChannels, 0.f);
    auto biasTensor = hasBias
                          ? file.loadF32(prefix + ".bias", device)
                          : Tensor::fromHostF32(zeros.data(), {outChannels}, device);

    return WNConv1dWeights {.flatWeight = std::move(flatWeightTensor),
                            .bias = std::move(biasTensor),
                            .hasBias = hasBias,
                            .inChannels = inChannels,
                            .outChannels = outChannels,
                            .kernelSize = kernelSize};
}

Tensor applyWNConv1d(ComputePass& pass, const Tensor& input, const WNConv1dWeights& weights, Device& device)
{
    auto unfolded = conv1dUnfoldGpu(pass, input, weights.inChannels, weights.kernelSize, device);

    return linear(pass, unfolded, weights.flatWeight, weights.hasBias ? &weights.bias : nullptr, device);
}
}
