#pragma once

#include <eacp/Core/Platform/Platform.h>
#include <eacp/Text/Text.h>
#include <NanoTest/NanoTest.h>

#include <initializer_list>
#include <string>

inline constexpr char32_t grinningFace = 0x1F600;
inline constexpr char32_t regionalIndicatorA = 0x1F1E6;
inline constexpr char32_t heavyBlackHeart = 0x2764;
inline constexpr char32_t copyrightSign = 0x00A9;

inline std::string utf8Of(std::initializer_list<char32_t> codepoints)
{
    auto text = std::string {};

    for (const auto codepoint: codepoints)
    {
        char encoded[4] = {};

        text.append(encoded,
                    (std::size_t) eacp::Text::encodeUtf8(codepoint, encoded));
    }

    return text;
}
