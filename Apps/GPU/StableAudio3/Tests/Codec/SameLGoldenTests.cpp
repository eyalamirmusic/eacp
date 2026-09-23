#include "GoldenFixture.h"

#include <eacp/Core/Utils/FilePath.h>
#include <eacp/GPU/Device/Device.h>
#include <eacp/GPU/Frame/ComputePass.h>
#include <eacp/ML/Kernels/Linear.h>
#include <eacp/ML/Loader/SafetensorsFile.h>
#include <Codec/CodecConfig.h>
#include <Codec/GpuOps.h>
#include <Codec/SA3Codec.h>
#include <Codec/SoftNormBottleneck.h>
#include <Codec/TransformerResamplingBlock.h>

#include <NanoTest/NanoTest.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <limits>
#include <optional>
#include <Checkpoints.h>

using namespace nano;
using namespace eacp;
using namespace eacp::GPU;
using namespace eacp::ML;
using namespace eacp::SA3Codec;
using namespace eacp::SA3Codec::Test;

namespace
{
constexpr auto patchedFrames = 128;
constexpr auto patchedChannels = 512;
constexpr auto encoderBlockFrames = 8;
constexpr auto encoderBlockDim = 1536;
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

float snrDb(const std::vector<float>& reference, const std::vector<float>& actual)
{
    auto signal = 0.0;
    auto noise = 0.0;

    for (auto i = std::size_t {}; i < reference.size(); ++i)
    {
        signal += (double) reference[i] * reference[i];
        auto diff = (double) actual[i] - reference[i];
        noise += diff * diff;
    }

    if (noise <= 0.0)
        return std::numeric_limits<float>::infinity();

    return (float) (10.0 * std::log10(signal / noise));
}

SameCodec loadSameL()
{
    auto file = SafetensorsFile::open(
        SA3Checkpoints::directory(SA3Checkpoints::sameL) / "model.safetensors");
    check(file.has_value());

    return SameCodec::loadFromSafetensors(
        *file, CodecConfig::sameL(), "", Device::shared());
}
} // namespace

auto tSameLEncoderResamplingBlockMatchesPython =
    test("SA3Codec/sameLEncoderResamplingBlockRawOutputMatchesPythonReference") = []
{
    auto& device = Device::shared();

    if (!device.isValid())
        return;

    auto codec = loadSameL();

    auto patched =
        loadGoldenFloats("sameL_patched_input", patchedFrames * patchedChannels);
    auto patchedTensor = Tensor::fromHostF32(
        patched.data(), {patchedFrames, patchedChannels}, device);

    auto blockOutput = std::optional<Tensor> {
        applyTransformerResamplingBlock(patchedTensor, codec.encoderBlock, device)};

    check(blockOutput->rows() == encoderBlockFrames);
    check(blockOutput->cols() == encoderBlockDim);

    auto expected = loadGoldenFloats("sameL_encoder_block_raw",
                                     encoderBlockFrames * encoderBlockDim);
    auto worst = maxAbsDifference(blockOutput->toHostF32(), expected);

    check(worst < 5.0e-3f);
};

auto tSameLDecoderResamplingBlockMatchesPython =
    test("SA3Codec/sameLDecoderResamplingBlockRawOutputMatchesPythonReference") = []
{
    auto& device = Device::shared();

    if (!device.isValid())
        return;

    auto codec = loadSameL();

    auto bottleneckDecoded =
        loadGoldenFloats("sameL_bottleneck_decoded", latentFrames * latentDim);
    auto bottleneckDecodedTensor = Tensor::fromHostF32(
        bottleneckDecoded.data(), {latentFrames, latentDim}, device);

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

    auto expected =
        loadGoldenFloats("sameL_decoder_raw", decoderRawFrames * decoderRawChannels);
    auto worst = maxAbsDifference(blockOutput->toHostF32(), expected);

    std::printf("sameLDecoderResamplingBlock worst=%f\n", worst);

    auto toleranceAboveNonReproducibleBottleneckNoiseAmplifiedByTheDecoder = 2.0e-2f;
    check(worst < toleranceAboveNonReproducibleBottleneckNoiseAmplifiedByTheDecoder);
};

auto tSameLBottleneckEncodeMatchesPython =
    test("SA3Codec/sameLBottleneckEncodeMatchesPythonReference") = []
{
    auto& device = Device::shared();

    if (!device.isValid())
        return;

    auto codec = loadSameL();

    auto projected =
        loadGoldenFloats("sameL_encoder_projected", latentFrames * latentDim);
    auto projectedTensor =
        Tensor::fromHostF32(projected.data(), {latentFrames, latentDim}, device);

    auto encoded = std::optional<Tensor> {};

    auto commands = device.makeCommandBuffer();
    {
        auto pass = commands.beginCompute();
        encoded = softNormBottleneckEncode(
            pass, projectedTensor, codec.bottleneck, device);
    }
    commands.commit();

    auto expected = loadGoldenFloats("sameL_latent", latentFrames * latentDim);
    auto worst = maxAbsDifference(encoded->toHostF32(), expected);

    check(worst < 1.0e-4f);
};

auto tSameLDecoderFoldAndLayer0Debug =
    test("SA3Codec/sameLDecoderFoldAndLayer0Debug") = []
{
    auto& device = Device::shared();

    if (!device.isValid())
        return;

    auto codec = loadSameL();

    auto bottleneckDecoded =
        loadGoldenFloats("sameL_bottleneck_decoded", latentFrames * latentDim);
    auto bottleneckDecodedTensor = Tensor::fromHostF32(
        bottleneckDecoded.data(), {latentFrames, latentDim}, device);

    auto folded = std::optional<Tensor> {};
    auto afterLayer0 = std::optional<Tensor> {};

    auto commands = device.makeCommandBuffer();
    {
        auto pass = commands.beginCompute();

        auto projected = linear(pass,
                                bottleneckDecodedTensor,
                                codec.decoderProjectionWeight,
                                &codec.decoderProjectionBias,
                                device);

        folded = foldWithNewTokens(
            pass, projected, 1, 16, codec.decoderBlock.newTokens, device);

        auto radius = codec.decoderBlock.slidingWindowRadiusChunks
                      * (codec.decoderBlock.stride + 1);
        auto band = AttentionBand {radius, radius, folded->rows()};

        afterLayer0 = applyCodecTransformerBlock(
            pass, *folded, codec.decoderBlock.layers[0], band, device);
    }
    commands.commit();

    check(folded->rows() == 136);
    check(folded->cols() == 1536);

    auto expectedFolded = loadGoldenFloats("sameL_decoder_folded", 136 * 1536);
    auto foldedWorst = maxAbsDifference(folded->toHostF32(), expectedFolded);
    std::printf("sameLDecoderFold worst=%f\n", foldedWorst);

    auto expectedAfterLayer0 =
        loadGoldenFloats("sameL_decoder_after_layer0", 136 * 1536);
    auto layer0Worst =
        maxAbsDifference(afterLayer0->toHostF32(), expectedAfterLayer0);
    std::printf("sameLDecoderAfterLayer0 worst=%f\n", layer0Worst);

    check(foldedWorst < 1.0e-3f);
    check(layer0Worst < 1.0e-2f);
};

auto tSameLRoundTripInPatchedDomainMatchesPython =
    test("SA3Codec/sameLRoundTripInPatchedDomainMatchesPythonReference") = []
{
    auto& device = Device::shared();

    if (!device.isValid())
        return;

    auto codec = loadSameL();

    auto patched =
        loadGoldenFloats("sameL_patched_input", patchedFrames * patchedChannels);
    auto patchedTensor = Tensor::fromHostF32(
        patched.data(), {patchedFrames, patchedChannels}, device);

    auto folded =
        applyTransformerResamplingBlock(patchedTensor, codec.encoderBlock, device);

    auto decoderProjected = std::optional<Tensor> {};

    auto commands = device.makeCommandBuffer();
    {
        auto pass = commands.beginCompute();

        auto projected = linear(pass,
                                folded,
                                codec.encoderProjectionWeight,
                                &codec.encoderProjectionBias,
                                device);
        auto latent =
            softNormBottleneckEncode(pass, projected, codec.bottleneck, device);
        auto denormalized =
            softNormBottleneckDecode(pass, latent, codec.bottleneck, device);
        decoderProjected = linear(pass,
                                  denormalized,
                                  codec.decoderProjectionWeight,
                                  &codec.decoderProjectionBias,
                                  device);
    }
    commands.commit();

    auto output = std::optional<Tensor> {applyTransformerResamplingBlock(
        *decoderProjected, codec.decoderBlock, device)};

    check(output->rows() == decoderRawFrames);
    check(output->cols() == decoderRawChannels);

    auto expected =
        loadGoldenFloats("sameL_full_output", decoderRawFrames * decoderRawChannels);
    auto snr = snrDb(expected, output->toHostF32());

    std::printf("sameLRoundTrip snr=%f\n", snr);
    check(snr > 20.0f);
};
