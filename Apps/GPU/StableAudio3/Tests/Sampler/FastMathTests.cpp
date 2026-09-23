#include <eacp/GPU/CommandBuffer/CommandBuffer.h>
#include <eacp/GPU/Device/Device.h>
#include <eacp/GPU/Frame/ComputePass.h>
#include <eacp/ML/Tensor/Tensor.h>
#include <Sampler/Ops.h>

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
float maxAbsoluteDifference(const std::vector<float>& a, const std::vector<float>& b)
{
    auto worst = 0.f;

    for (auto i = std::size_t {}; i < a.size(); ++i)
        worst = std::max(worst, std::abs(a[i] - b[i]));

    return worst;
}
} // namespace

auto tScaleAndAddMatchesReference =
    test("SA3Sampler/scaleAndAddMatchesHandComputedFormula") = []
{
    auto& device = Device::shared();

    if (!device.isValid())
        return;

    auto aValues = std::vector<float> {1.f, 2.f, 3.f, 4.f};
    auto bValues = std::vector<float> {10.f, 20.f, 30.f, 40.f};

    auto a = Tensor::fromHostF32(aValues.data(), {1, 4}, device);
    auto b = Tensor::fromHostF32(bValues.data(), {1, 4}, device);

    auto commands = device.makeCommandBuffer();
    auto result = Tensor::uninitializedF32({1, 4}, device);

    {
        auto pass = commands.beginCompute();
        result = scaleAndAdd(pass, a, 0.25f, b, -0.5f, device);
    }

    commands.commit();

    auto expected = std::vector<float> {};

    for (auto i = std::size_t {}; i < aValues.size(); ++i)
        expected.push_back(aValues[i] * 0.25f + bValues[i] * -0.5f);

    check(maxAbsoluteDifference(result.toHostF32(), expected) <= 1.0e-5f);
};

auto tScaleAndAddPingpongIdentityAtFinalStep =
    test("SA3Sampler/scaleAndAddReducesToDenoisedWhenNextSigmaIsZero") = []
{
    auto& device = Device::shared();

    if (!device.isValid())
        return;

    auto denoisedValues = std::vector<float> {0.1f, -0.2f, 0.3f, -0.4f};
    auto noiseValues = std::vector<float> {5.f, 6.f, 7.f, 8.f};

    auto denoised = Tensor::fromHostF32(denoisedValues.data(), {1, 4}, device);
    auto noise = Tensor::fromHostF32(noiseValues.data(), {1, 4}, device);

    auto commands = device.makeCommandBuffer();
    auto result = Tensor::uninitializedF32({1, 4}, device);

    {
        auto pass = commands.beginCompute();
        result = scaleAndAdd(pass, denoised, 1.f - 0.f, noise, 0.f, device);
    }

    commands.commit();

    check(maxAbsoluteDifference(result.toHostF32(), denoisedValues) <= 1.0e-6f);
};
