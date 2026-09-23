#include <TextEncoder/Tokenizer/BpeTokenizer.h>
#include <Checkpoints.h>

#include <NanoTest/NanoTest.h>

using namespace nano;
using namespace eacp::SA3TextEncoder;

namespace
{
std::string tokenizerJsonPath()
{
    return (eacp::SA3Checkpoints::directory(eacp::SA3Checkpoints::smallMusic)
            / "t5gemma-b-b-ul2/tokenizer.json")
        .str();
}

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
} // namespace

auto tTokenizerLoads =
    test("SA3TextEncoder/Tokenizer/loadsFromRealTokenizerJson") = []
{
    auto tokenizer = BpeTokenizer::load(tokenizerJsonPath());
    check(tokenizer.has_value());
};

auto tTokenizerSingleWord =
    test("SA3TextEncoder/Tokenizer/singleWordMatchesHardcodedIds") = []
{
    auto tokenizer = BpeTokenizer::load(tokenizerJsonPath());

    if (!tokenizer.has_value())
        return;

    checkEncoding(*tokenizer, "hello", {17534}, 1);
};

auto tTokenizerShortPhrase =
    test("SA3TextEncoder/Tokenizer/shortPhraseMatchesHardcodedIds") = []
{
    auto tokenizer = BpeTokenizer::load(tokenizerJsonPath());

    if (!tokenizer.has_value())
        return;

    checkEncoding(*tokenizer, "lofi house loop", {545, 2485, 3036, 10273}, 4);
};

auto tTokenizerPadsToMaxLength =
    test("SA3TextEncoder/Tokenizer/padsToExactlyMaxLength") = []
{
    auto tokenizer = BpeTokenizer::load(tokenizerJsonPath());

    if (!tokenizer.has_value())
        return;

    auto result = tokenizer->encode("hello", 32);
    check(result.ids.size() == 32);
    check(result.validLength == 1);
    check(result.ids[0] == 17534);

    for (auto i = std::size_t {1}; i < result.ids.size(); ++i)
        check(result.ids[i] == 0);
};
