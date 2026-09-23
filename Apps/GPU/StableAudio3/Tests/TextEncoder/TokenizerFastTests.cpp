#include <TextEncoder/Tokenizer/BpeTokenizer.h>

#include <NanoTest/NanoTest.h>

using namespace nano;
using namespace eacp::SA3TextEncoder;

namespace
{
constexpr auto tokenizerJsonPath =
    "/Users/jamiepond/.cache/huggingface/hub/"
    "models--stabilityai--stable-audio-3-small-music/snapshots/"
    "0fef1392cd842149a2b6d445e181c97608faac06/t5gemma-b-b-ul2/tokenizer.json";

void checkEncoding(const BpeTokenizer& tokenizer,
                   const std::string& text,
                   const std::vector<int>& expectedIds,
                   int expectedValidLength)
{
    auto result = tokenizer.encode(text, 256);

    check(result.validLength == expectedValidLength);
    check(result.ids.size() == 256);

    for (auto i = std::size_t {0}; i < expectedIds.size(); ++i)
        check(result.ids[i] == expectedIds[i]);

    for (auto i = expectedIds.size(); i < result.ids.size(); ++i)
        check(result.ids[i] == 0);
}
}

auto tTokenizerLoads = test("SA3TextEncoder/Tokenizer/loadsFromRealTokenizerJson") = []
{
    auto tokenizer = BpeTokenizer::load(tokenizerJsonPath);
    check(tokenizer.has_value());
};

auto tTokenizerSingleWord = test("SA3TextEncoder/Tokenizer/singleWordMatchesHardcodedIds") = []
{
    auto tokenizer = BpeTokenizer::load(tokenizerJsonPath);

    if (!tokenizer.has_value())
        return;

    checkEncoding(*tokenizer, "hello", {17534}, 1);
};

auto tTokenizerShortPhrase = test("SA3TextEncoder/Tokenizer/shortPhraseMatchesHardcodedIds") = []
{
    auto tokenizer = BpeTokenizer::load(tokenizerJsonPath);

    if (!tokenizer.has_value())
        return;

    checkEncoding(*tokenizer, "lofi house loop", {545, 2485, 3036, 10273}, 4);
};

auto tTokenizerPadsToMaxLength = test("SA3TextEncoder/Tokenizer/padsToExactlyMaxLength") = []
{
    auto tokenizer = BpeTokenizer::load(tokenizerJsonPath);

    if (!tokenizer.has_value())
        return;

    auto result = tokenizer->encode("hello", 32);
    check(result.ids.size() == 32);
    check(result.validLength == 1);
    check(result.ids[0] == 17534);

    for (auto i = std::size_t {1}; i < result.ids.size(); ++i)
        check(result.ids[i] == 0);
};
