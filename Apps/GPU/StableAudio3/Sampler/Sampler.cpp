#include "Sampler.h"

#include "Ops.h"

#include <eacp/GPU/Codegen/KernelCache.h>
#include <eacp/GPU/CommandBuffer/CommandBuffer.h>
#include <eacp/GPU/Frame/ComputePass.h>

#include <memory>
#include <random>

namespace eacp::SA3Sampler
{
using namespace eacp::GPU;
using namespace eacp::ML;

NoiseSource randomNoiseSource(std::uint64_t seed)
{
    auto engine = std::make_shared<std::mt19937_64>(seed);
    auto distribution = std::make_shared<std::normal_distribution<float>>(0.f, 1.f);

    return [engine, distribution](int count)
    {
        auto values = std::vector<float>((std::size_t) count);

        for (auto& value: values)
            value = (*distribution)(*engine);

        return values;
    };
}

namespace
{
Tensor uploadNoise(const NoiseSource& noiseSource, int rows, int columns, Device& device)
{
    auto values = noiseSource(rows * columns);
    return Tensor::fromHostF32(values.data(), {rows, columns}, device);
}
}

Tensor pingpongSampleWithModel(const ModelForward& model,
                               int latentRows,
                               int latentColumns,
                               int steps,
                               const NoiseSource& noiseSource,
                               Device& device)
{
    auto x = uploadNoise(noiseSource, latentRows, latentColumns, device);

    for (auto i = 0; i < steps; ++i)
    {
        auto tCurr = 1.f - (float) i / (float) steps;
        auto tNext = 1.f - (float) (i + 1) / (float) steps;

        auto noise = uploadNoise(noiseSource, latentRows, latentColumns, device);
        auto commands = device.makeCommandBuffer();

        {
            auto pass = commands.beginCompute();

            auto velocity = model(pass, x, tCurr);
            auto denoised = scaleAndAdd(pass, x, 1.f, velocity, -tCurr, device);

            x = scaleAndAdd(pass, denoised, 1.f - tNext, noise, tNext, device);
        }

        commands.commit();
    }

    return x;
}

Tensor pingpongSample(const SA3DiT::Weights& weights,
                      const Tensor& crossAttnContext,
                      int latentLength,
                      float secondsTotal,
                      int steps,
                      const NoiseSource& noiseSource,
                      Device& device)
{
    auto model = [&](ComputePass& pass, const Tensor& x, float timestep)
    {
        return SA3DiT::forward(pass, weights, x, timestep, secondsTotal, crossAttnContext, device);
    };

    return pingpongSampleWithModel(
        model, latentLength, SA3DiT::ioChannels, steps, noiseSource, device);
}
}
