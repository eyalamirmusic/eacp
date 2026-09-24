#include <NanoTest/NanoTest.h>

#include <eacp/GPU/CommandBuffer/CommandBuffer.h>
#include <eacp/GPU/Device/Device.h>
#include <eacp/GPU/Frame/ComputePass.h>
#include <DiT/Ops.h>
#include <DiT/SA3DiT.h>

#include <cmath>
#include <vector>

using namespace nano;
using namespace eacp;
using namespace eacp::GPU;
using namespace eacp::ML;
using namespace eacp::SA3DiT;

namespace
{
void checkClose(const std::vector<float>& actual,
                const std::vector<float>& expected,
                float tolerance)
{
    check(actual.size() == expected.size());

    for (auto i = std::size_t {}; i < expected.size(); ++i)
        check(std::abs(actual[i] - expected[i]) <= tolerance);
}
} // namespace

auto tExpoFourierFeaturesConstantFrequency =
    test("SA3DiT/expoFourierFeaturesMatchesReferenceAtConstantFrequency") = []
{
    auto& device = Device::shared();

    if (!device.isValid())
        return;

    auto commands = device.makeCommandBuffer();
    auto features = Tensor::uninitializedF32({1, 2}, device);

    {
        auto pass = commands.beginCompute();
        features = expoFourierFeatures(pass, 0.25f, 2, 1.0f, 1.0f, device);
    }

    commands.commit();

    checkClose(features.toHostF32(), {0.f, 1.f}, 1.0e-5f);
};

auto tExpoFourierFeaturesZeroValue =
    test("SA3DiT/expoFourierFeaturesIsAllCosOneAtZero") = []
{
    auto& device = Device::shared();

    if (!device.isValid())
        return;

    auto commands = device.makeCommandBuffer();
    auto features = Tensor::uninitializedF32({1, 4}, device);

    {
        auto pass = commands.beginCompute();
        features = expoFourierFeatures(pass, 0.f, 4, 0.5f, 10000.f, device);
    }

    commands.commit();

    checkClose(features.toHostF32(), {1.f, 1.f, 0.f, 0.f}, 1.0e-5f);
};

auto tAdaLNModulate = test("SA3DiT/adaLNModulateMatchesReference") = []
{
    auto& device = Device::shared();

    if (!device.isValid())
        return;

    auto x = Tensor::fromHostF32(
        std::vector<float> {1.f, 2.f, 3.f, 4.f}.data(), {2, 2}, device);
    auto scale =
        Tensor::fromHostF32(std::vector<float> {0.f, 1.f}.data(), {2}, device);
    auto shift =
        Tensor::fromHostF32(std::vector<float> {10.f, 0.f}.data(), {2}, device);

    auto commands = device.makeCommandBuffer();
    auto result = Tensor::uninitializedF32({2, 2}, device);

    {
        auto pass = commands.beginCompute();
        result = adaLNModulate(pass, x, scale, shift, device);
    }

    commands.commit();

    checkClose(result.toHostF32(), {11.f, 4.f, 13.f, 8.f}, 1.0e-4f);
};

auto tSigmoidGate = test("SA3DiT/sigmoidGateMatchesReference") = []
{
    auto& device = Device::shared();

    if (!device.isValid())
        return;

    auto x =
        Tensor::fromHostF32(std::vector<float> {2.f, 4.f}.data(), {1, 2}, device);
    auto gate =
        Tensor::fromHostF32(std::vector<float> {0.f, 100.f}.data(), {2}, device);

    auto commands = device.makeCommandBuffer();
    auto result = Tensor::uninitializedF32({1, 2}, device);

    {
        auto pass = commands.beginCompute();
        result = sigmoidGate(pass, x, gate, device);
    }

    commands.commit();

    auto expectedFirst = 2.f * (1.f / (1.f + std::exp(-1.f)));
    checkClose(result.toHostF32(), {expectedFirst, 0.f}, 1.0e-3f);
};

auto tAddBroadcastRow = test("SA3DiT/addBroadcastRowSkipsRowsBeforeStart") = []
{
    auto& device = Device::shared();

    if (!device.isValid())
        return;

    auto x = Tensor::fromHostF32(
        std::vector<float> {1.f, 1.f, 2.f, 2.f, 3.f, 3.f}.data(), {3, 2}, device);
    auto addend =
        Tensor::fromHostF32(std::vector<float> {10.f, 20.f}.data(), {2}, device);

    auto commands = device.makeCommandBuffer();
    auto result = Tensor::uninitializedF32({3, 2}, device);

    {
        auto pass = commands.beginCompute();
        result = addBroadcastRow(pass, x, addend, 1, device);
    }

    commands.commit();

    checkClose(result.toHostF32(), {1.f, 1.f, 12.f, 22.f, 13.f, 23.f}, 1.0e-5f);
};

auto tSqueezeTrailingUnitDim =
    test("SA3DiT/squeezeTrailingUnitDimDropsKernelDim") = []
{
    auto& device = Device::shared();

    if (!device.isValid())
        return;

    auto x = Tensor::fromHostF32(
        std::vector<float> {1.f, 2.f, 3.f, 4.f}.data(), {2, 2, 1}, device);
    auto squeezed = squeezeTrailingUnitDim(std::move(x));

    check(squeezed.rank() == 2);
    check(squeezed.dim(0) == 2);
    check(squeezed.dim(1) == 2);
};
