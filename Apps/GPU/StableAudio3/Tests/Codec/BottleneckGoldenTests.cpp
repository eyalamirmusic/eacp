#include "GoldenFixture.h"

#include <eacp/Core/Utils/FilePath.h>
#include <eacp/GPU/Device/Device.h>
#include <eacp/GPU/Frame/ComputePass.h>
#include <eacp/ML/Loader/SafetensorsFile.h>
#include <Codec/SA3Codec.h>
#include <Codec/SoftNormBottleneck.h>

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

Tensor runBottleneck(Device& device,
                     const std::vector<float>& input,
                     const SoftNormBottleneckWeights& weights,
                     bool encode)
{
    auto inputTensor = Tensor::fromHostF32(input.data(), {latentFrames, latentDim}, device);
    auto result = std::optional<Tensor> {};

    auto commands = device.makeCommandBuffer();
    {
        auto pass = commands.beginCompute();
        result = encode ? softNormBottleneckEncode(pass, inputTensor, weights, device)
                        : softNormBottleneckDecode(pass, inputTensor, weights, device);
    }
    commands.commit();

    return std::move(*result);
}
}

auto tSoftNormBottleneckEncodeMatchesPython =
    test("SA3Codec/softNormBottleneckEncodeMatchesPythonReference") = []
{
    auto& device = Device::shared();

    if (!device.isValid())
        return;

    auto file = SafetensorsFile::open(FilePath {SA3_CODEC_CHECKPOINT_PATH});
    check(file.has_value());

    if (!file.has_value())
        return;

    auto codec = SameCodec::loadFromSafetensors(*file, "pretransform.model", device);

    auto encoderProjected = loadGoldenFloats("encoder_projected", latentFrames * latentDim);
    auto latent = runBottleneck(device, encoderProjected, codec.bottleneck, true);

    auto expected = loadGoldenFloats("latent", latentFrames * latentDim);

    check(maxAbsDifference(latent.toHostF32(), expected) < 1.0e-4f);
};

auto tSoftNormBottleneckDecodeMatchesPython =
    test("SA3Codec/softNormBottleneckDecodeMatchesPythonReference") = []
{
    auto& device = Device::shared();

    if (!device.isValid())
        return;

    auto file = SafetensorsFile::open(FilePath {SA3_CODEC_CHECKPOINT_PATH});
    check(file.has_value());

    if (!file.has_value())
        return;

    auto codec = SameCodec::loadFromSafetensors(*file, "pretransform.model", device);

    auto latentGolden = loadGoldenFloats("latent", latentFrames * latentDim);
    auto decoded = runBottleneck(device, latentGolden, codec.bottleneck, false);

    auto expected = loadGoldenFloats("bottleneck_decoded", latentFrames * latentDim);

    auto toleranceAboveNonReproducibleInferenceNoiseRegularization = 1.0e-3f;
    check(maxAbsDifference(decoded.toHostF32(), expected)
          < toleranceAboveNonReproducibleInferenceNoiseRegularization);
};
