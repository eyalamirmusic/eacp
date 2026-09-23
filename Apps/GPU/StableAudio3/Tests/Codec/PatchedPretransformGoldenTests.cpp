#include "GoldenFixture.h"

#include <Codec/PatchedPretransform.h>

#include <NanoTest/NanoTest.h>

#include <cmath>
#include <limits>

using namespace nano;
using namespace eacp::SA3Codec;
using namespace eacp::SA3Codec::Test;

namespace
{
constexpr auto sampleCount = 32768;
constexpr auto patchedFrames = 128;
constexpr auto patchedChannels = 512;

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
} // namespace

auto tPatchedPretransformEncodeMatchesPython =
    test("SA3Codec/patchedPretransformEncodeMatchesPythonReference") = []
{
    auto left = loadGoldenFloats("input_left", sampleCount);
    auto right = loadGoldenFloats("input_right", sampleCount);
    auto expected = loadGoldenFloats("patched", patchedFrames * patchedChannels);

    auto waveform = StereoWaveform {left, right};
    auto patched = patchedPretransformEncode(waveform);

    check(patched.rows == patchedFrames);
    check(patched.cols == patchedChannels);
    check(maxAbsDifference(patched.data, expected) < 1.0e-5f);
};

auto tPatchedPretransformDecodeMatchesPythonOnRealDecoderOutput =
    test("SA3Codec/patchedPretransformDecodeMatchesPythonOnRealDecoderOutput") = []
{
    constexpr auto decoderRawFrames = 128;
    constexpr auto decoderRawChannels = 512;

    auto decoderRaw = HostMatrix {
        loadGoldenFloats("decoder_raw", decoderRawFrames * decoderRawChannels),
        decoderRawFrames,
        decoderRawChannels};

    auto decoded = patchedPretransformDecode(decoderRaw, sampleCount);

    auto expectedLeft = loadGoldenFloats("output_left", sampleCount);
    auto expectedRight = loadGoldenFloats("output_right", sampleCount);

    check((int) decoded.left.size() == sampleCount);
    check(maxAbsDifference(decoded.left, expectedLeft) < 1.0e-5f);
    check(maxAbsDifference(decoded.right, expectedRight) < 1.0e-5f);
};

auto tPatchedPretransformDecodeInvertsEncode =
    test("SA3Codec/patchedPretransformDecodeInvertsEncodeOnGoldenInput") = []
{
    auto left = loadGoldenFloats("input_left", sampleCount);
    auto right = loadGoldenFloats("input_right", sampleCount);

    auto waveform = StereoWaveform {left, right};
    auto patched = patchedPretransformEncode(waveform);
    auto roundTripped = patchedPretransformDecode(patched, sampleCount);

    check((int) roundTripped.left.size() == sampleCount);
    check(maxAbsDifference(roundTripped.left, left) < 1.0e-6f);
    check(maxAbsDifference(roundTripped.right, right) < 1.0e-6f);
};
