#pragma once

#include <cstddef>
#include <string_view>

// UTF-8 in and out, for the seams of this module that speak codepoints: the
// atlas shaping one codepoint on its own, and the platform shapers walking a
// string into the UTF-16 their text engines take.
namespace eacp::Text
{
// Writes `codepoint` as UTF-8 into `into`, which holds at least four bytes,
// and returns how many it used.
inline std::size_t encodeUtf8(char32_t codepoint, char* into)
{
    if (codepoint < 0x80)
    {
        into[0] = static_cast<char>(codepoint);
        return 1;
    }

    if (codepoint < 0x800)
    {
        into[0] = static_cast<char>(0xC0 | (codepoint >> 6));
        into[1] = static_cast<char>(0x80 | (codepoint & 0x3F));
        return 2;
    }

    if (codepoint < 0x10000)
    {
        into[0] = static_cast<char>(0xE0 | (codepoint >> 12));
        into[1] = static_cast<char>(0x80 | ((codepoint >> 6) & 0x3F));
        into[2] = static_cast<char>(0x80 | (codepoint & 0x3F));
        return 3;
    }

    into[0] = static_cast<char>(0xF0 | (codepoint >> 18));
    into[1] = static_cast<char>(0x80 | ((codepoint >> 12) & 0x3F));
    into[2] = static_cast<char>(0x80 | ((codepoint >> 6) & 0x3F));
    into[3] = static_cast<char>(0x80 | (codepoint & 0x3F));
    return 4;
}

// Decodes one UTF-8 sequence starting at `index`, advancing it past what was
// consumed. Malformed bytes yield U+FFFD and advance by one, so a bad byte
// costs one replacement glyph rather than desynchronising the rest of the line.
inline char32_t decodeUtf8(std::string_view text, std::size_t& index)
{
    const auto lead = static_cast<unsigned char>(text[index]);

    const auto continuationBytes = lead < 0x80   ? 0
                                   : lead < 0xC0 ? -1
                                   : lead < 0xE0 ? 1
                                   : lead < 0xF0 ? 2
                                   : lead < 0xF8 ? 3
                                                 : -1;

    if (continuationBytes < 0 || index + continuationBytes >= text.size())
    {
        ++index;
        return 0xFFFD;
    }

    constexpr char32_t leadMask[] = {0x7F, 0x1F, 0x0F, 0x07};
    auto codepoint = static_cast<char32_t>(lead & leadMask[continuationBytes]);

    for (auto i = 1; i <= continuationBytes; ++i)
    {
        const auto byte = static_cast<unsigned char>(text[index + i]);

        if ((byte & 0xC0) != 0x80)
        {
            ++index;
            return 0xFFFD;
        }

        codepoint = (codepoint << 6) | (byte & 0x3F);
    }

    index += continuationBytes + 1;
    return codepoint;
}
} // namespace eacp::Text
