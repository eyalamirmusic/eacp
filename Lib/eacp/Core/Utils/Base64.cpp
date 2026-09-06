#include "Base64.h"

#include <array>
#include <cstdint>

namespace eacp::Base64
{
namespace
{
constexpr auto alphabet = std::string_view(
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/");

constexpr int invalid = -1;

constexpr std::array<int, 256> makeReverseAlphabet()
{
    auto table = std::array<int, 256>();

    for (auto& entry: table)
        entry = invalid;

    for (auto i = 0; i < (int) alphabet.size(); ++i)
        table[(unsigned char) alphabet[(std::size_t) i]] = i;

    return table;
}

constexpr auto reverseAlphabet = makeReverseAlphabet();

std::uint32_t byteAt(std::string_view bytes, std::size_t index)
{
    return (std::uint8_t) bytes[index];
}

std::uint32_t groupAt(std::string_view bytes, std::size_t index, std::size_t left)
{
    auto group = byteAt(bytes, index) << 16;

    if (left > 1)
        group |= byteAt(bytes, index + 1) << 8;

    if (left > 2)
        group |= byteAt(bytes, index + 2);

    return group;
}

int sextetAt(std::string_view text, std::size_t index)
{
    return reverseAlphabet[(unsigned char) text[index]];
}

std::size_t paddingLength(std::string_view text)
{
    if (text.size() < 4 || text[text.size() - 1] != '=')
        return 0;

    return text[text.size() - 2] == '=' ? 2 : 1;
}

bool appendDecodedQuad(std::string& out, std::string_view quad, std::size_t padding)
{
    auto group = std::uint32_t {0};

    for (auto i = std::size_t {0}; i < 4 - padding; ++i)
    {
        auto sextet = sextetAt(quad, i);

        if (sextet == invalid)
            return false;

        group |= (std::uint32_t) sextet << (18 - 6 * i);
    }

    for (auto i = std::size_t {0}; i < 3 - padding; ++i)
        out.push_back((char) ((group >> (16 - 8 * i)) & 0xFF));

    return true;
}
} // namespace

std::string encode(std::string_view bytes)
{
    auto encoded = std::string();
    encoded.reserve(((bytes.size() + 2) / 3) * 4);

    for (auto i = std::size_t {0}; i < bytes.size(); i += 3)
    {
        auto left = bytes.size() - i;
        auto group = groupAt(bytes, i, left);

        encoded.push_back(alphabet[(group >> 18) & 0x3F]);
        encoded.push_back(alphabet[(group >> 12) & 0x3F]);
        encoded.push_back(left > 1 ? alphabet[(group >> 6) & 0x3F] : '=');
        encoded.push_back(left > 2 ? alphabet[group & 0x3F] : '=');
    }

    return encoded;
}

std::optional<std::string> decode(std::string_view text)
{
    if (text.size() % 4 != 0)
        return std::nullopt;

    auto padding = paddingLength(text);

    auto decoded = std::string();
    decoded.reserve(text.size() / 4 * 3);

    for (auto i = std::size_t {0}; i < text.size(); i += 4)
    {
        auto isLastQuad = i + 4 == text.size();

        if (!appendDecodedQuad(decoded, text.substr(i, 4), isLastQuad ? padding : 0))
            return std::nullopt;
    }

    return decoded;
}
} // namespace eacp::Base64
