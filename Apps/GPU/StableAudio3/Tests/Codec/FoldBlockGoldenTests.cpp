#include "GoldenFixture.h"

#include <eacp/Core/Utils/FilePath.h>
#include <eacp/GPU/Device/Device.h>
#include <eacp/GPU/Frame/ComputePass.h>
#include <eacp/ML/Kernels/Linear.h>
#include <eacp/ML/Loader/SafetensorsFile.h>
#include <Codec/PatchedPretransform.h>
#include <Codec/SA3Codec.h>
#include <Codec/TransformerResamplingBlock.h>

#include <NanoTest/NanoTest.h>

#include <algorithm>
#include <cmath>
#include <limits>
#include <optional>

using namespace nano;
using namespace eacp;
using namespace eacp::GPU;
using namespace eacp::ML;
using namespace eacp::SA3Codec;
using namespace eacp::SA3Codec::Test;

namespace
{
constexpr auto sampleCount = 32768;
constexpr auto patchedFrames = 128;
constexpr auto patchedChannels = 512;
constexpr auto encoderBlockFrames = 8;
constexpr auto encoderBlockDim = 768;
constexpr auto latentFrames = 8;
constexpr auto latentDim = 256;
constexpr auto decoderRawFrames = 128;
constexpr auto decoderRawChannels = 512;

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
}

auto tEncoderResamplingBlockMatchesPython =
    test("SA3Codec/encoderResamplingBlockRawOutputMatchesPythonReference") = []
{
    auto& device = Device::shared();

    if (!device.isValid())
        return;

    auto file = SafetensorsFile::open(FilePath {SA3_CODEC_CHECKPOINT_PATH});
    check(file.has_value());

    if (!file.has_value())
        return;

    auto codec =
        SameCodec::loadFromSafetensors(*file, CodecConfig::sameS(), "pretransform.model", device);

    auto left = loadGoldenFloats("input_left", sampleCount);
    auto right = loadGoldenFloats("input_right", sampleCount);
    auto patched = patchedPretransformEncode(StereoWaveform {left, right});

    check(patched.rows == patchedFrames);
    check(patched.cols == patchedChannels);

    auto patchedTensor = Tensor::fromHostF32(patched.data.data(), {patched.rows, patched.cols}, device);
    auto blockOutput = std::optional<Tensor> {
        applyTransformerResamplingBlock(patchedTensor, codec.encoderBlock, device)};

    check(blockOutput->rows() == encoderBlockFrames);
    check(blockOutput->cols() == encoderBlockDim);

    auto expected = loadGoldenFloats("encoder_block_raw", encoderBlockFrames * encoderBlockDim);
    auto worst = maxAbsDifference(blockOutput->toHostF32(), expected);

    check(worst < 5.0e-3f);
};

auto tDecoderResamplingBlockMatchesPython =
    test("SA3Codec/decoderResamplingBlockRawOutputMatchesPythonReference") = []
{
    auto& device = Device::shared();

    if (!device.isValid())
        return;

    auto file = SafetensorsFile::open(FilePath {SA3_CODEC_CHECKPOINT_PATH});
    check(file.has_value());

    if (!file.has_value())
        return;

    auto codec =
        SameCodec::loadFromSafetensors(*file, CodecConfig::sameS(), "pretransform.model", device);

    auto bottleneckDecoded = loadGoldenFloats("bottleneck_decoded", latentFrames * latentDim);
    auto bottleneckDecodedTensor =
        Tensor::fromHostF32(bottleneckDecoded.data(), {latentFrames, latentDim}, device);

    auto projected = std::optional<Tensor> {};

    auto commands = device.makeCommandBuffer();
    {
        auto pass = commands.beginCompute();

        projected = linear(pass,
                          bottleneckDecodedTensor,
                          codec.decoderProjectionWeight,
                          &codec.decoderProjectionBias,
                          device);
    }
    commands.commit();

    auto blockOutput = std::optional<Tensor> {
        applyTransformerResamplingBlock(*projected, codec.decoderBlock, device)};

    check(blockOutput->rows() == decoderRawFrames);
    check(blockOutput->cols() == decoderRawChannels);

    auto expected = loadGoldenFloats("decoder_raw", decoderRawFrames * decoderRawChannels);
    auto worst = maxAbsDifference(blockOutput->toHostF32(), expected);

    auto toleranceAboveNonReproducibleBottleneckNoiseAmplifiedByTheDecoder = 2.0e-2f;
    check(worst < toleranceAboveNonReproducibleBottleneckNoiseAmplifiedByTheDecoder);
};
