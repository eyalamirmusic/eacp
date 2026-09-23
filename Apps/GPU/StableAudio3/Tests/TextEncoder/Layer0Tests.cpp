#include "GoldenIO.h"

#include <eacp/Core/Utils/Files.h>
#include <eacp/GPU/CommandBuffer/CommandBuffer.h>
#include <eacp/GPU/Device/Device.h>
#include <eacp/GPU/Frame/ComputePass.h>
#include <eacp/ML/Loader/Json.h>
#include <Checkpoints.h>
#include <TextEncoder/Encoder/T5GemmaEncoder.h>

#include <NanoTest/NanoTest.h>

#include <algorithm>
#include <cmath>

using namespace nano;
using namespace eacp;
using namespace eacp::GPU;
using namespace eacp::ML;
using namespace eacp::SA3TextEncoder;

namespace
{
std::string t5gemmaWeightsPath()
{
    return (eacp::SA3Checkpoints::directory(eacp::SA3Checkpoints::smallMusic)
            / "t5gemma-b-b-ul2/model.safetensors")
        .str();
}

#ifndef SA3_TEXT_ENCODER_GOLDEN_DIR
#define SA3_TEXT_ENCODER_GOLDEN_DIR "."
#endif

float maxAbsoluteDifference(const std::vector<float>& a, const std::vector<float>& b)
{
    auto worst = 0.f;

    for (auto i = std::size_t {0}; i < a.size(); ++i)
        worst = std::max(worst, std::abs(a[i] - b[i]));

    return worst;
}
} // namespace

auto tLayer0MatchesGolden =
    test("SA3TextEncoder/Encoder/layer0MatchesGoldenReference") = []
{
    auto& device = Device::shared();

    if (!device.isValid())
        return;

    auto encoder = T5GemmaEncoder::load(t5gemmaWeightsPath(), device);
    check(encoder.has_value());

    if (!encoder.has_value())
        return;

    auto goldenPath = std::string {SA3_TEXT_ENCODER_GOLDEN_DIR} + "/golden.json";
    auto goldenText = Files::readFile(FilePath {goldenPath});
    auto parsed = Json::parse(goldenText);
    check(parsed.has_value());

    if (!parsed.has_value())
        return;

    auto index = 0;

    for (const auto& entry: parsed->asArray())
    {
        auto validLength = (int) entry.find("valid_length")->asNumber();
        auto ids = std::vector<int> {};

        for (const auto& idValue: entry.find("input_ids")->asArray())
            ids.push_back((int) idValue.asNumber());

        std::optional<Tensor> result;
        auto commands = device.makeCommandBuffer();

        {
            auto pass = commands.beginCompute();
            result =
                encoder->encodeTokensThroughLayer(pass, ids, validLength, 1, device);
        }

        commands.commit();

        auto actual = result->toHostF32();
        auto expected =
            Test::readBinaryFloats(std::string {SA3_TEXT_ENCODER_GOLDEN_DIR}
                                       + "/layer0_" + std::to_string(index) + ".bin",
                                   actual.size());

        auto worst = maxAbsoluteDifference(actual, expected);
        check(worst < 0.25f);

        ++index;
    }
};
