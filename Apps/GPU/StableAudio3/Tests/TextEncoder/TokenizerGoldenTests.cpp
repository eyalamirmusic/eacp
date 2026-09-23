#include <eacp/Core/Utils/Files.h>
#include <eacp/ML/Loader/Json.h>
#include <TextEncoder/Tokenizer/BpeTokenizer.h>

#include <NanoTest/NanoTest.h>

using namespace nano;
using namespace eacp;
using namespace eacp::SA3TextEncoder;

namespace
{
constexpr auto tokenizerJsonPath =
    "/Users/jamiepond/.cache/huggingface/hub/"
    "models--stabilityai--stable-audio-3-small-music/snapshots/"
    "0fef1392cd842149a2b6d445e181c97608faac06/t5gemma-b-b-ul2/tokenizer.json";

#ifndef SA3_TEXT_ENCODER_GOLDEN_DIR
#    define SA3_TEXT_ENCODER_GOLDEN_DIR "."
#endif
}

auto tTokenizerMatchesRealTokenizerOnGoldenPrompts =
    test("SA3TextEncoder/Tokenizer/matchesRealTokenizerOnGoldenPrompts") = []
{
    auto tokenizer = BpeTokenizer::load(tokenizerJsonPath);
    check(tokenizer.has_value());

    if (!tokenizer.has_value())
        return;

    auto goldenPath =
        std::string {SA3_TEXT_ENCODER_GOLDEN_DIR} + "/golden.json";
    auto goldenText = Files::readFile(FilePath {goldenPath});
    check(!goldenText.empty());

    auto parsed = ML::Json::parse(goldenText);
    check(parsed.has_value());

    if (!parsed.has_value())
        return;

    for (const auto& entry: parsed->asArray())
    {
        auto prompt = entry.find("prompt")->asString();
        auto expectedValidLength = (int) entry.find("valid_length")->asNumber();
        const auto& expectedIds = entry.find("input_ids")->asArray();

        auto result = tokenizer->encode(prompt, (int) expectedIds.size());

        check(result.validLength == expectedValidLength);

        for (auto i = std::size_t {0}; i < expectedIds.size(); ++i)
            check(result.ids[i] == (int) expectedIds[i].asNumber());
    }
};
