#include "GoldenIO.h"

#include <eacp/Core/Utils/Files.h>
#include <eacp/GPU/CommandBuffer/CommandBuffer.h>
#include <eacp/GPU/Device/Device.h>
#include <eacp/GPU/Frame/ComputePass.h>
#include <eacp/ML/Loader/Json.h>
#include <eacp/SA3TextEncoder/Encoder/T5GemmaEncoder.h>
#include <eacp/SA3TextEncoder/SA3TextEncoder.h>

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
constexpr auto t5gemmaWeightsPath =
    "/Users/jamiepond/.cache/huggingface/hub/"
    "models--stabilityai--stable-audio-3-small-music/snapshots/"
    "0fef1392cd842149a2b6d445e181c97608faac06/t5gemma-b-b-ul2/model.safetensors";

constexpr auto tokenizerJsonPath =
    "/Users/jamiepond/.cache/huggingface/hub/"
    "models--stabilityai--stable-audio-3-small-music/snapshots/"
    "0fef1392cd842149a2b6d445e181c97608faac06/t5gemma-b-b-ul2/tokenizer.json";

constexpr auto conditionerWeightsPath =
    "/Users/jamiepond/.cache/huggingface/hub/"
    "models--stabilityai--stable-audio-3-small-music/snapshots/"
    "0fef1392cd842149a2b6d445e181c97608faac06/model.safetensors";

#ifndef SA3_TEXT_ENCODER_GOLDEN_DIR
#    define SA3_TEXT_ENCODER_GOLDEN_DIR "."
#endif

float maxAbsoluteDifference(const std::vector<float>& a, const std::vector<float>& b)
{
    auto worst = 0.f;

    for (auto i = std::size_t {0}; i < a.size(); ++i)
        worst = std::max(worst, std::abs(a[i] - b[i]));

    return worst;
}
}

auto tFullEncoderMatchesGolden =
    test("SA3TextEncoder/Encoder/fullStackMatchesGoldenReference") = []
{
    auto& device = Device::shared();

    if (!device.isValid())
        return;

    auto encoder = T5GemmaEncoder::load(t5gemmaWeightsPath, device);
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
            result = encoder->encodeTokens(pass, ids, validLength, device);
        }

        commands.commit();

        auto actual = result->toHostF32();
        auto expected = Test::readBinaryFloats(
            std::string {SA3_TEXT_ENCODER_GOLDEN_DIR} + "/final_" + std::to_string(index)
                + ".bin",
            actual.size());

        auto worst = maxAbsoluteDifference(actual, expected);
        check(worst < 0.5f);

        ++index;
    }
};

auto tPaddingEmbeddingSubstitutesPaddedRows =
    test("SA3TextEncoder/Model/paddingEmbeddingSubstitutesPaddedRows") = []
{
    auto& device = Device::shared();

    if (!device.isValid())
        return;

    auto model = SA3TextEncoderModel::load(
        tokenizerJsonPath, t5gemmaWeightsPath, conditionerWeightsPath, device);
    check(model.has_value());

    if (!model.has_value())
        return;

    std::optional<PromptEncoding> encoding;
    auto commands = device.makeCommandBuffer();

    {
        auto pass = commands.beginCompute();
        encoding = model->encodePrompt(pass, "hello", device);
    }

    commands.commit();

    check(encoding->validLength == 1);

    auto host = encoding->embeddings.toHostF32();
    auto padding = Test::readBinaryFloats(
        std::string {SA3_TEXT_ENCODER_GOLDEN_DIR} + "/padding_embedding.bin", 768);

    auto paddedRow = std::vector<float>(host.begin() + 768, host.begin() + 1536);
    auto worst = maxAbsoluteDifference(paddedRow, padding);

    check(worst < 1.0e-5f);
};
