#include <eacp/GPU/CommandBuffer/CommandBuffer.h>
#include <eacp/GPU/Device/Device.h>
#include <eacp/GPU/Frame/ComputePass.h>
#include <eacp/ML/Tensor/Tensor.h>
#include <eacp/ML/Kernels/TensorOps.h>
#include <Sampler/Sampler.h>

#include <NanoTest/NanoTest.h>

#include <cmath>
#include <vector>

using namespace nano;
using namespace eacp;
using namespace eacp::GPU;
using namespace eacp::ML;
using namespace eacp::SA3Sampler;

namespace
{
constexpr auto latentColumns = 4;
constexpr auto velocityGain = 0.5f;
constexpr auto fixedNoiseValue = 2.f;

double
    referenceFinalValue(float initialValue, int steps, float gain, float noiseValue)
{
    auto x = (double) initialValue;

    for (auto i = 0; i < steps; ++i)
    {
        auto tCurr = 1.0 - (double) i / (double) steps;
        auto tNext = 1.0 - (double) (i + 1) / (double) steps;

        auto velocity = gain * x;
        auto denoised = x - tCurr * velocity;

        x = (1.0 - tNext) * denoised + tNext * (double) noiseValue;
    }

    return x;
}
} // namespace

auto tPingpongMatchesHandDerivedRecurrence =
    test("SA3Sampler/pingpongWithLinearModelMatchesHandDerivedRecurrence") = []
{
    auto& device = Device::shared();

    if (!device.isValid())
        return;

    auto constantNoise = [](int count)
    { return std::vector<float>((std::size_t) count, fixedNoiseValue); };

    auto model = [&](ComputePass& pass, const Tensor& x, float)
    { return scaleAndAdd(pass, x, velocityGain, x, 0.f, device); };

    constexpr auto steps = 3;

    auto result = pingpongSampleWithModel(
        model, 1, latentColumns, steps, constantNoise, device);

    auto actual = result.toHostF32();
    auto expected =
        referenceFinalValue(fixedNoiseValue, steps, velocityGain, fixedNoiseValue);

    auto worst = 0.f;

    for (auto value: actual)
        worst = std::max(worst, std::abs(value - (float) expected));

    check(worst <= 1.0e-4f);
};

auto tPingpongLastStepEqualsDenoisedWithZeroTail =
    test("SA3Sampler/pingpongFinalStepDropsToDenoisedAsSigmaReachesZero") = []
{
    auto& device = Device::shared();

    if (!device.isValid())
        return;

    auto constantNoise = [](int count)
    { return std::vector<float>((std::size_t) count, 7.f); };

    auto zeroVelocityModel = [&](ComputePass& pass, const Tensor& x, float)
    { return scaleAndAdd(pass, x, 0.f, x, 0.f, device); };

    auto result = pingpongSampleWithModel(
        zeroVelocityModel, 1, latentColumns, 1, constantNoise, device);

    auto actual = result.toHostF32();

    for (auto value: actual)
        check(std::abs(value - 7.f) <= 1.0e-5f);
};
