#pragma once

#include "../Common.h"
#include <eacp/Core/Geometry/Geometry.h>

#include <cstdint>
#include <initializer_list>

namespace eacp::Graphics
{
using Point = Geometry::Point<float>;
using Insets = Geometry::Insets<float>;
using Rect = Geometry::Rectangle<float>;

// Corners bigger than the rect they round are out of contract for the platform
// path builders, so every rounded-rect call site fits the radius first.
float clampedCornerRadius(const Rect& rect, float radius);

// Defined inline and constexpr so themes can be compile-time constants: a
// palette is a table of named colors, and building one at static-init time
// costs nothing and lets it live in rodata.
struct Color
{
    constexpr Color() = default;

    constexpr Color(float rToUse, float gToUse, float bToUse, float aToUse = 1.f)
        : r(rToUse)
        , g(gToUse)
        , b(bToUse)
        , a(aToUse)
    {
    }

    static constexpr Color gray(float value, float alpha = 1.f)
    {
        return {value, value, value, alpha};
    }

    static constexpr Color white(float alpha = 1.f)
    {
        return {1.f, 1.f, 1.f, alpha};
    }
    static constexpr Color black(float alpha = 1.f)
    {
        return {0.f, 0.f, 0.f, alpha};
    }

    // Fully transparent black: what a fill that should draw nothing holds, and
    // the far end of a fade to nothing.
    static constexpr Color transparent() { return {0.f, 0.f, 0.f, 0.f}; }

    // The pure primaries and secondaries, each channel at 0 or 1. Note that
    // green is the full-intensity (0, 1, 0) -- the colour CSS and JUCE call
    // "lime" -- and not their darker 0x008000 "green".
    static constexpr Color red(float alpha = 1.f) { return {1.f, 0.f, 0.f, alpha}; }
    static constexpr Color green(float alpha = 1.f)
    {
        return {0.f, 1.f, 0.f, alpha};
    }
    static constexpr Color blue(float alpha = 1.f) { return {0.f, 0.f, 1.f, alpha}; }
    static constexpr Color yellow(float alpha = 1.f)
    {
        return {1.f, 1.f, 0.f, alpha};
    }
    static constexpr Color cyan(float alpha = 1.f) { return {0.f, 1.f, 1.f, alpha}; }
    static constexpr Color magenta(float alpha = 1.f)
    {
        return {1.f, 0.f, 1.f, alpha};
    }

    // 8-bit channels, 0-255 each -- what a design tool or a colour picker hands
    // over.
    static constexpr Color fromRGBA8(std::uint8_t red8,
                                     std::uint8_t green8,
                                     std::uint8_t blue8,
                                     std::uint8_t alpha8 = 255)
    {
        return {static_cast<float>(red8) / 255.f,
                static_cast<float>(green8) / 255.f,
                static_cast<float>(blue8) / 255.f,
                static_cast<float>(alpha8) / 255.f};
    }

    // A packed 0xAARRGGBB, the layout juce::Colour takes. The alpha byte is
    // part of the value, so 0xff0000 is a *transparent* red -- write
    // 0xffff0000, or use fromRGB for a colour without one.
    static constexpr Color fromARGB(std::uint32_t argb)
    {
        return fromRGBA8(static_cast<std::uint8_t>((argb >> 16) & 0xff),
                         static_cast<std::uint8_t>((argb >> 8) & 0xff),
                         static_cast<std::uint8_t>(argb & 0xff),
                         static_cast<std::uint8_t>((argb >> 24) & 0xff));
    }

    // A packed opaque 0xRRGGBB, the way a colour is written in CSS. Any bits
    // above the low 24 are ignored.
    static constexpr Color fromRGB(std::uint32_t rgb, float alpha = 1.f)
    {
        return fromARGB(rgb | 0xff000000u).withAlpha(alpha);
    }

    // The inverse of fromARGB: each channel clamped to [0, 1] and rounded to
    // the nearest byte.
    constexpr std::uint32_t toARGB() const
    {
        return (toByte(a) << 24) | (toByte(r) << 16) | (toByte(g) << 8) | toByte(b);
    }

    constexpr Color withAlpha(float alpha) const { return {r, g, b, alpha}; }

    // The straight blend from this colour (0) to `other` (1), alpha included,
    // as juce::Colour::interpolatedWith. `proportion` is clamped to [0, 1], so
    // an overshooting animation curve cannot push a channel out of range.
    constexpr Color interpolatedWith(const Color& other, float proportion) const
    {
        auto t = proportion < 0.f ? 0.f : (proportion > 1.f ? 1.f : proportion);

        return {r + (other.r - r) * t,
                g + (other.g - g) * t,
                b + (other.b - b) * t,
                a + (other.a - a) * t};
    }

    Color brighter(float amount = 0.1f) const;
    Color darker(float amount = 0.1f) const;

    float r = 0.f;
    float g = 0.f;
    float b = 0.f;
    float a = 1.f;

private:
    static constexpr std::uint32_t toByte(float channel)
    {
        auto clamped = channel < 0.f ? 0.f : (channel > 1.f ? 1.f : channel);
        return static_cast<std::uint32_t>(clamped * 255.f + 0.5f);
    }
};

struct GradientStop
{
    Color color;
    float position = 0.f;
};

struct LinearGradient
{
    LinearGradient() = default;
    LinearGradient(Point startToUse,
                   Point endToUse,
                   std::initializer_list<GradientStop> stopsToUse);

    Point start;
    Point end;
    Vector<GradientStop> stops;
};

} // namespace eacp::Graphics
