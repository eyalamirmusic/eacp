#pragma once

#include <eacp/Graphics/Primitives/Primitives.h>

#include <array>
#include <cstdint>

namespace term
{
// Packed 0xRRGGBB. Cells store resolved colors; the Default* attr bits mark
// cells that follow the theme's default fg/bg so inverse video and future
// theme switches stay faithful.
using Rgb = std::uint32_t;

namespace Attr
{
constexpr std::uint16_t Bold = 1 << 0;
constexpr std::uint16_t Faint = 1 << 1;
constexpr std::uint16_t Italic = 1 << 2;
constexpr std::uint16_t Underline = 1 << 3;
constexpr std::uint16_t Blink = 1 << 4;
constexpr std::uint16_t Inverse = 1 << 5;
constexpr std::uint16_t Hidden = 1 << 6;
constexpr std::uint16_t Strike = 1 << 7;
constexpr std::uint16_t Wide = 1 << 8;
constexpr std::uint16_t WideCont = 1 << 9;
constexpr std::uint16_t DefaultFg = 1 << 10;
constexpr std::uint16_t DefaultBg = 1 << 11;
} // namespace Attr

struct Cell
{
    char32_t ch = U' ';
    Rgb fg = 0;
    Rgb bg = 0;
    std::uint16_t attrs = Attr::DefaultFg | Attr::DefaultBg;
};

inline eacp::Graphics::Color toColor(Rgb rgb, float alpha = 1.0f)
{
    return {(float) ((rgb >> 16) & 0xff) / 255.0f,
            (float) ((rgb >> 8) & 0xff) / 255.0f,
            (float) (rgb & 0xff) / 255.0f,
            alpha};
}

struct Theme
{
    Rgb background = 0x1a1b26;
    Rgb foreground = 0xc0caf5;
    Rgb cursor = 0xc0caf5;
    Rgb selection = 0x33467c;

    std::array<Rgb, 16> ansi = {0x15161e,
                                0xf7768e,
                                0x9ece6a,
                                0xe0af68,
                                0x7aa2f7,
                                0xbb9af7,
                                0x7dcfff,
                                0xa9b1d6,
                                0x414868,
                                0xf7768e,
                                0x9ece6a,
                                0xe0af68,
                                0x7aa2f7,
                                0xbb9af7,
                                0x7dcfff,
                                0xc0caf5};

    Rgb indexed(int index) const
    {
        if (index < 0)
            return foreground;

        if (index < 16)
            return ansi[(std::size_t) index];

        if (index < 232)
        {
            constexpr std::array<Rgb, 6> steps = {0, 95, 135, 175, 215, 255};
            const auto n = index - 16;
            const auto r = steps[(std::size_t) (n / 36)];
            const auto g = steps[(std::size_t) ((n / 6) % 6)];
            const auto b = steps[(std::size_t) (n % 6)];
            return (r << 16) | (g << 8) | b;
        }

        if (index < 256)
        {
            const auto v = (Rgb) (8 + 10 * (index - 232));
            return (v << 16) | (v << 8) | v;
        }

        return foreground;
    }
};

// East-Asian/emoji width classes the renderer and screen model agree on:
// 0 = combining/zero-width, 2 = double cell, 1 = everything else.
int charWidth(char32_t cp);
} // namespace term
