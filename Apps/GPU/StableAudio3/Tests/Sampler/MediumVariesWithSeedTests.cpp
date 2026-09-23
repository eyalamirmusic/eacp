#include <NanoTest/NanoTest.h>

#include <eacp/Core/Utils/FilePath.h>
#include <eacp/GPU/Device/Device.h>
#include <eacp/ML/Loader/SafetensorsFile.h>
#include <DiT/SA3DiT.h>
#include <DiT/Weights.h>
#include <Sampler/Sampler.h>

#include <cmath>
#include <random>
#include <Checkpoints.h>

using namespace nano;
using namespace eacp;
using namespace eacp::GPU;
using namespace eacp::ML;
using namespace eacp::SA3DiT;
using namespace eacp::SA3Sampler;

namespace
{
bool hasNonFinite(const std::vector<float>& values)
{
    for (auto value: values)
        if (!std::isfinite(value))
            return true;

    return false;
}

float maxAbsDiff(const std::vector<float>& a, const std::vector<float>& b)
{
    auto worst = 0.f;

    for (auto i = std::size_t {}; i < a.size(); ++i)
        worst = std::max(worst, std::abs(a[i] - b[i]));

    return worst;
}
} // namespace

auto tMediumVariesWithSeed = test("SA3Sampler/mediumFullLoopVariesWithSeed") = []
{
    auto& device = Device::shared();

    if (!device.isValid())
        return;

    auto file = SafetensorsFile::open(
        SA3Checkpoints::directory(SA3Checkpoints::medium) / "model.safetensors");

    if (!file.has_value())
        return;

    auto config = DiTConfig::medium();
    auto weights = loadWeights(*file, config, device);

    constexpr auto latentLength = 130;
    constexpr auto contextLen = 256;
    auto condTokenDimC = config.condTokenDim;

    auto rng = std::mt19937_64 {99};
    auto dist = std::normal_distribution<float> {0.f, 1.f};
    auto contextValues =
        std::vector<float>((std::size_t) (contextLen * condTokenDimC));

    for (auto& value: contextValues)
        value = dist(rng);

    auto contextTensor = Tensor::fromHostF32(
        contextValues.data(), {contextLen, condTokenDimC}, device);

    auto runFullLoop = [&](std::uint64_t seed)
    {
        return pingpongSample(weights,
                              contextTensor,
                              latentLength,
                              8.f,
                              8,
                              randomNoiseSource(seed),
                              device)
            .toHostF32();
    };

    auto latentSeedA = runFullLoop(1);
    auto latentSeedB = runFullLoop(2);

    check(!hasNonFinite(latentSeedA));
    check(!hasNonFinite(latentSeedB));
    check(maxAbsDiff(latentSeedA, latentSeedB) > 1.0e-4f);
};
