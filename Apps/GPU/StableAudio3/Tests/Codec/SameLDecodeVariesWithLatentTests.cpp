#include <NanoTest/NanoTest.h>

#include <eacp/Core/Utils/FilePath.h>
#include <eacp/GPU/Device/Device.h>
#include <eacp/ML/Loader/SafetensorsFile.h>
#include <Codec/SA3Codec.h>

#include <cmath>
#include <random>
#include <Checkpoints.h>

using namespace nano;
using namespace eacp;
using namespace eacp::GPU;
using namespace eacp::ML;
using namespace eacp::SA3Codec;

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
    auto n = std::min(a.size(), b.size());

    for (auto i = std::size_t {}; i < n; ++i)
    {
        auto gap = std::abs(a[i] - b[i]);

        if (!std::isfinite(gap))
            return gap;

        worst = std::max(worst, gap);
    }

    return worst;
}

void checkDecodeVariesAtLength(SameCodec& codec, Device& device, int latentLength)
{
    constexpr auto latentDim = 256;
    auto sampleCount = latentLength * 4096;

    auto latentA = randomVector(latentLength * latentDim, 11);
    auto latentB = randomVector(latentLength * latentDim, 22);

    auto latentTensorA =
        Tensor::fromHostF32(latentA.data(), {latentLength, latentDim}, device);
    auto latentTensorB =
        Tensor::fromHostF32(latentB.data(), {latentLength, latentDim}, device);

    auto waveformA = codec.decode(latentTensorA, sampleCount, device);
    auto waveformB = codec.decode(latentTensorB, sampleCount, device);

    check(maxAbsDiff(waveformA.left, waveformB.left) > 1.0e-4f);
    check(maxAbsDiff(waveformA.right, waveformB.right) > 1.0e-4f);
}
} // namespace

auto tSameLDecodeVariesAtSmallScale =
    test("SA3Codec/sameLDecodeVariesWithLatentAtSmallScale") = []
{
    auto& device = Device::shared();

    if (!device.isValid())
        return;

    auto file = SafetensorsFile::open(
        SA3Checkpoints::directory(SA3Checkpoints::sameL) / "model.safetensors");

    if (!file.has_value())
        return;

    auto codec =
        SameCodec::loadFromSafetensors(*file, CodecConfig::sameL(), "", device);
    checkDecodeVariesAtLength(codec, device, 8);
};

auto tSameLDecodeVariesAtRealClipScale =
    test("SA3Codec/sameLDecodeVariesWithLatentAtRealClipScale") = []
{
    auto& device = Device::shared();

    if (!device.isValid())
        return;

    auto file = SafetensorsFile::open(
        SA3Checkpoints::directory(SA3Checkpoints::sameL) / "model.safetensors");

    if (!file.has_value())
        return;

    auto codec =
        SameCodec::loadFromSafetensors(*file, CodecConfig::sameL(), "", device);
    checkDecodeVariesAtLength(codec, device, 130);
};
