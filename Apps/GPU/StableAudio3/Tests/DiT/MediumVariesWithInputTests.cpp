#include <NanoTest/NanoTest.h>
#include <Tests/SkipWithoutCheckpoint.h>

#include <eacp/Core/Utils/FilePath.h>
#include <eacp/GPU/CommandBuffer/CommandBuffer.h>
#include <eacp/GPU/Device/Device.h>
#include <eacp/GPU/Frame/ComputePass.h>
#include <eacp/ML/Loader/SafetensorsFile.h>
#include <DiT/SA3DiT.h>
#include <DiT/Weights.h>

#include <random>
#include <Checkpoints.h>

using namespace nano;
using namespace eacp;
using namespace eacp::GPU;
using namespace eacp::ML;
using namespace eacp::SA3DiT;

namespace
{
std::vector<float> randomVector(int count, std::uint64_t seed)
{
    auto rng = std::mt19937_64 {seed};
    auto dist = std::normal_distribution<float> {0.f, 1.f};
    auto values = std::vector<float>((std::size_t) count);

    for (auto& value: values)
        value = dist(rng);

    return values;
}

float maxAbsDiff(const std::vector<float>& a, const std::vector<float>& b)
{
    auto worst = 0.f;

    for (auto i = std::size_t {}; i < a.size(); ++i)
        worst = std::max(worst, std::abs(a[i] - b[i]));

    return worst;
}
} // namespace

auto tMediumInputSensitivity = test("SA3DiT/mediumForwardVariesWithInput") = []
{
    auto& device = Device::shared();

    if (!device.isValid())
        return;

    if (SA3Checkpoints::skipWithoutCheckpoint(SA3Checkpoints::medium,
                                              "model.safetensors"))
        return;

    auto file = SafetensorsFile::open(
        SA3Checkpoints::directory(SA3Checkpoints::medium) / "model.safetensors");

    if (!file.has_value())
        return;

    auto config = DiTConfig::medium();
    auto weights = loadWeights(*file, config, device);

    constexpr auto latentLength = 8;
    constexpr auto contextLen = 5;
    auto ioChannelsC = config.ioChannels;
    auto condTokenDimC = config.condTokenDim;

    auto latentA = randomVector(latentLength * ioChannelsC, 1);
    auto latentB = randomVector(latentLength * ioChannelsC, 2);
    auto contextA = randomVector(contextLen * condTokenDimC, 3);
    auto contextB = randomVector(contextLen * condTokenDimC, 4);

    auto runOnce = [&](const std::vector<float>& latent,
                       const std::vector<float>& context,
                       float timestep) -> std::vector<float>
    {
        auto latentTensor =
            Tensor::fromHostF32(latent.data(), {latentLength, ioChannelsC}, device);
        auto contextTensor =
            Tensor::fromHostF32(context.data(), {contextLen, condTokenDimC}, device);

        auto commands = device.makeCommandBuffer();
        auto out = Tensor::uninitializedF32({latentLength, ioChannelsC}, device);

        {
            auto pass = commands.beginCompute();
            out = forward(
                pass, weights, latentTensor, timestep, 20.f, contextTensor, device);
        }

        commands.commit();
        return out.toHostF32();
    };

    auto baseline = runOnce(latentA, contextA, 0.5f);
    auto differentLatent = runOnce(latentB, contextA, 0.5f);
    auto differentContext = runOnce(latentA, contextB, 0.5f);
    auto differentTimestep = runOnce(latentA, contextA, 0.9f);

    check(maxAbsDiff(baseline, differentLatent) > 1.0e-4f);
    check(maxAbsDiff(baseline, differentContext) > 1.0e-4f);
    check(maxAbsDiff(baseline, differentTimestep) > 1.0e-4f);
};
