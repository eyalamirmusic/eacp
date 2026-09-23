#include "BpeTokenizer.h"

#include <eacp/Core/Utils/Files.h>
#include <eacp/ML/Loader/Json.h>

#include <cstdio>

namespace eacp::SA3TextEncoder
{
namespace
{
constexpr auto spaceGlyph = "\xE2\x96\x81";

std::string normalized(const std::string& text)
{
    auto result = std::string {};
    result.reserve(text.size());

    for (auto character: text)
    {
        if (character == ' ')
            result += spaceGlyph;
        else
            result.push_back(character);
    }

    return result;
}

int utf8Length(unsigned char leadByte)
{
    if ((leadByte & 0x80u) == 0x00u)
        return 1;
    if ((leadByte & 0xE0u) == 0xC0u)
        return 2;
    if ((leadByte & 0xF0u) == 0xE0u)
        return 3;
    if ((leadByte & 0xF8u) == 0xF0u)
        return 4;
    return 1;
}

std::vector<std::string> codepoints(const std::string& text)
{
    auto result = std::vector<std::string> {};
    auto i = std::size_t {0};

    while (i < text.size())
    {
        auto length = utf8Length((unsigned char) text[i]);

        if (i + (std::size_t) length > text.size())
            length = 1;

        result.push_back(text.substr(i, (std::size_t) length));
        i += (std::size_t) length;
    }

    return result;
}

std::string byteFallbackToken(unsigned char byte)
{
    char buffer[8];
    std::snprintf(buffer, sizeof(buffer), "<0x%02X>", (unsigned) byte);
    return std::string {buffer};
}

std::string mergeKey(const std::string& left, const std::string& right)
{
    return left + '\x1f' + right;
}
}

std::optional<BpeTokenizer> BpeTokenizer::load(const std::string& tokenizerJsonPath)
{
    auto text = Files::readFile(FilePath {tokenizerJsonPath});

    if (text.empty())
        return std::nullopt;

    auto parsed = ML::Json::parse(text);

    if (!parsed.has_value())
        return std::nullopt;

    auto model = parsed->find("model");

    if (model == nullptr)
        return std::nullopt;

    auto vocabValue = model->find("vocab");
    auto mergesValue = model->find("merges");

    if (vocabValue == nullptr || mergesValue == nullptr)
        return std::nullopt;

    auto tokenizer = BpeTokenizer {};

    for (const auto& [token, idValue]: vocabValue->asObject())
        tokenizer.vocab[token] = (int) idValue.asNumber();

    auto rank = 0;

    for (const auto& pair: mergesValue->asArray())
    {
        const auto& parts = pair.asArray();

        if (parts.size() == 2)
            tokenizer.mergeRank[mergeKey(parts[0].asString(), parts[1].asString())] =
                rank;

        ++rank;
    }

    auto padEntry = tokenizer.vocab.find("<pad>");
    tokenizer.padTokenId = padEntry != tokenizer.vocab.end() ? padEntry->second : 0;

    return tokenizer;
}

std::vector<std::string> BpeTokenizer::symbolsFor(const std::string& normalizedText) const
{
    auto result = std::vector<std::string> {};

    for (auto& codepoint: codepoints(normalizedText))
    {
        if (vocab.find(codepoint) != vocab.end())
        {
            result.push_back(codepoint);
            continue;
        }

        for (auto byte: codepoint)
            result.push_back(byteFallbackToken((unsigned char) byte));
    }

    return result;
}

void BpeTokenizer::mergeSymbols(std::vector<std::string>& symbols) const
{
    while (symbols.size() > 1)
    {
        auto bestIndex = std::optional<std::size_t> {};
        auto bestRank = 0;

        for (auto i = std::size_t {0}; i + 1 < symbols.size(); ++i)
        {
            auto entry = mergeRank.find(mergeKey(symbols[i], symbols[i + 1]));

            if (entry == mergeRank.end())
                continue;

            if (!bestIndex.has_value() || entry->second < bestRank)
            {
                bestIndex = i;
                bestRank = entry->second;
            }
        }

        if (!bestIndex.has_value())
            break;

        auto i = *bestIndex;
        symbols[i] += symbols[i + 1];
        symbols.erase(symbols.begin() + (std::ptrdiff_t) i + 1);
    }
}

int BpeTokenizer::idFor(const std::string& symbol) const
{
    auto entry = vocab.find(symbol);

    if (entry != vocab.end())
        return entry->second;

    auto unk = vocab.find("<unk>");
    return unk != vocab.end() ? unk->second : 0;
}

TokenizeResult BpeTokenizer::encode(const std::string& text, int maxLength) const
{
    auto symbols = symbolsFor(normalized(text));
    mergeSymbols(symbols);

    auto result = TokenizeResult {};
    result.ids.assign((std::size_t) maxLength, padTokenId);

    auto count = std::min((int) symbols.size(), maxLength);

    for (auto i = 0; i < count; ++i)
        result.ids[(std::size_t) i] = idFor(symbols[(std::size_t) i]);

    result.validLength = count;
    return result;
}
}
