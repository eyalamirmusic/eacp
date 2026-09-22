#include "GoldenFixture.h"

#include <eacp/Core/Utils/FilePath.h>
#include <eacp/GPU/Device/Device.h>
#include <eacp/ML/Loader/SafetensorsFile.h>
#include <eacp/SA3Codec/SA3Codec.h>

#include <NanoTest/NanoTest.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <limits>

using namespace nano;
using namespace eacp;
using namespace eacp::GPU;
using namespace eacp::ML;
using namespace eacp::SA3Codec;
using namespace eacp::SA3Codec::Test;

namespace
{
constexpr auto sampleCount = 32768;
constexpr auto latentFrames = 8;
constexpr auto latentDim = 256;

float maxAbsDifference(const std::vector<float>& a, const std::vector<float>& b)
{
    auto worst = 0.f;

    for (auto i = std::size_t {}; i < a.size(); ++i)
    {
        auto difference = std::abs(a[i] - b[i]);

        if (std::isnan(difference))
            return std::numeric_limits<float>::infinity();

        worst = std::max(worst, difference);
    }

    return worst;
}

float signalToNoiseRatioDb(const std::vector<float>& reference, const std::vector<float>& candidate)
{
    auto signalPower = 0.0;
    auto noisePower = 0.0;

    for (auto i = std::size_t {}; i < reference.size(); ++i)
    {
        auto ref = (double) reference[i];
        auto diff = (double) reference[i] - (double) candidate[i];
        signalPower += ref * ref;
        noisePower += diff * diff;
    }

    if (noisePower <= 0.0)
        return 1000.f;

    return (float) (10.0 * std::log10(signalPower / noisePower));
}
}

auto tSameCodecEncodeMatchesPythonLatent =
    test("SA3Codec/sameCodecEncodeMatchesPythonFullLatent") = []
{
    auto& device = Device::shared();

    if (!device.isValid())
        return;

    auto file = SafetensorsFile::open(FilePath {SA3_CODEC_CHECKPOINT_PATH});
    check(file.has_value());

    if (!file.has_value())
        return;

    auto codec = SameCodec::loadFromSafetensors(*file, "pretransform.model", device);

    auto left = loadGoldenFloats("input_left", sampleCount);
    auto right = loadGoldenFloats("input_right", sampleCount);

    auto latent = codec.encode(StereoWaveform {left, right}, device);
    auto expected = loadGoldenFloats("full_latent", latentFrames * latentDim);

    check(latent.rows() == latentFrames);
    check(latent.cols() == latentDim);
    check(maxAbsDifference(latent.toHostF32(), expected) < 5.0e-3f);
};

auto tSameCodecDecodeAloneMatchesPythonWaveform =
    test("SA3Codec/sameCodecDecodeAloneMatchesPythonWaveformGivenGoldenLatent") = []
{
    auto& device = Device::shared();

    if (!device.isValid())
        return;

    auto file = SafetensorsFile::open(FilePath {SA3_CODEC_CHECKPOINT_PATH});
    check(file.has_value());

    if (!file.has_value())
        return;

    auto codec = SameCodec::loadFromSafetensors(*file, "pretransform.model", device);

    auto fullLatent = loadGoldenFloats("full_latent", latentFrames * latentDim);
    auto latentTensor = Tensor::fromHostF32(fullLatent.data(), {latentFrames, latentDim}, device);

    auto decoded = codec.decode(latentTensor, sampleCount, device);

    auto expectedLeft = loadGoldenFloats("full_output_left", sampleCount);
    auto expectedRight = loadGoldenFloats("full_output_right", sampleCount);

    auto snrLeft = signalToNoiseRatioDb(expectedLeft, decoded.left);
    auto snrRight = signalToNoiseRatioDb(expectedRight, decoded.right);

    std::printf("decodeAlone snrLeft=%g snrRight=%g\n", (double) snrLeft, (double) snrRight);

    check(snrLeft > 20.f);
    check(snrRight > 20.f);
};

auto tSameCodecRoundTripMatchesPythonWaveform =
    test("SA3Codec/sameCodecEncodeDecodeMatchesPythonWaveform") = []
{
    auto& device = Device::shared();

    if (!device.isValid())
        return;

    auto file = SafetensorsFile::open(FilePath {SA3_CODEC_CHECKPOINT_PATH});
    check(file.has_value());

    if (!file.has_value())
        return;

    auto codec = SameCodec::loadFromSafetensors(*file, "pretransform.model", device);

    auto left = loadGoldenFloats("input_left", sampleCount);
    auto right = loadGoldenFloats("input_right", sampleCount);

    auto latent = codec.encode(StereoWaveform {left, right}, device);
    auto decoded = codec.decode(latent, sampleCount, device);

    check((int) decoded.left.size() == sampleCount);

    auto expectedLeft = loadGoldenFloats("full_output_left", sampleCount);
    auto expectedRight = loadGoldenFloats("full_output_right", sampleCount);

    auto snrLeft = signalToNoiseRatioDb(expectedLeft, decoded.left);
    auto snrRight = signalToNoiseRatioDb(expectedRight, decoded.right);

    check(snrLeft > 20.f);
    check(snrRight > 20.f);
};

auto tSameCodecRoundTripPreservesInputSignal =
    test("SA3Codec/sameCodecEncodeDecodePreservesRecognizableSignal") = []
{
    auto& device = Device::shared();

    if (!device.isValid())
        return;

    auto file = SafetensorsFile::open(FilePath {SA3_CODEC_CHECKPOINT_PATH});
    check(file.has_value());

    if (!file.has_value())
        return;

    auto codec = SameCodec::loadFromSafetensors(*file, "pretransform.model", device);

    auto left = loadGoldenFloats("input_left", sampleCount);
    auto right = loadGoldenFloats("input_right", sampleCount);

    auto latent = codec.encode(StereoWaveform {left, right}, device);
    auto decoded = codec.decode(latent, sampleCount, device);

    auto snrLeft = signalToNoiseRatioDb(left, decoded.left);
    auto snrRight = signalToNoiseRatioDb(right, decoded.right);

    check(snrLeft > 3.f);
    check(snrRight > 3.f);
};
