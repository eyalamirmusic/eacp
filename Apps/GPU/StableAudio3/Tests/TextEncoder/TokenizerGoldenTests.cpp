#include <eacp/Core/Utils/Files.h>
#include <eacp/ML/Loader/Json.h>
#include <TextEncoder/Tokenizer/BpeTokenizer.h>
#include <Checkpoints.h>

#include <NanoTest/NanoTest.h>

using namespace nano;
using namespace eacp;
using namespace eacp::SA3TextEncoder;

namespace
{
std::string tokenizerJsonPath()
{
    return (eacp::SA3Checkpoints::directory(eacp::SA3Checkpoints::smallMusic)
            / "t5gemma-b-b-ul2/tokenizer.json")
        .str();
}

#ifndef SA3_TEXT_ENCODER_GOLDEN_DIR
#define SA3_TEXT_ENCODER_GOLDEN_DIR "."
#endif
} // namespace

auto tTokenizerMatchesRealTokenizerOnGoldenPrompts =
    test("SA3TextEncoder/Tokenizer/matchesRealTokenizerOnGoldenPrompts") = []
{
    auto tokenizer = BpeTokenizer::load(tokenizerJsonPath());
    check(tokenizer.has_value());

    if (!tokenizer.has_value())
        return;

    auto goldenPath = std::string {SA3_TEXT_ENCODER_GOLDEN_DIR} + "/golden.json";
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
