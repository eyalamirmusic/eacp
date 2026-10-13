#pragma once

#include "../Common.h"

#include <cstdint>
#include <initializer_list>

namespace eacp::Graphics
{
struct Point
{
    Point() = default;
    Point(float xToUse, float yToUse);

    float length() const;
    float distanceTo(const Point& other) const;
    Point normalized() const;

    float x = 0.f;
    float y = 0.f;
};

Point operator+(const Point& a, const Point& b);

Point operator-(const Point& a, const Point& b);

// A border of four thicknesses, one per edge, in points. Every side defaults
// to zero so a caller names only the edges it means: `Insets {.top = 56.f}`.
struct Insets
{
    // The width and height the four sides add up to.
    Point size() const;

    float top = 0.f;
    float left = 0.f;
    float bottom = 0.f;
    float right = 0.f;
};

// An axis-aligned rectangle in a **y-down** space: y grows downward, so y = 0
// is the top edge and `fromTop` returns the slice against it.
//
// That is what the rest of eacp means by y. The backing views set isFlipped, so
// View::setBounds and MouseEvent::pos are both measured from the top, and the
// sprite, glyph and scissor paths all map y = 0 to the top of their target. The
// splitters below came from JUCE, where views really are y-up, and keeping that
// sense here meant `removeFromTop` handed you the bottom slice — a header laid
// out with it landed along the bottom edge of its window, which is silent
// because it still compiles and still draws something.
struct Rect
{
    Rect() = default;
    Rect(float xToUse, float yToUse, float wToUse, float hToUse);

    Rect getRelative(const Rect& ratio) const;
    Point getRelativePoint(const Point& point) const;

    bool contains(const Point& point) const;

    // A rect with no area. Both an explicitly empty rect and one whose width or
    // height has gone negative — which is what oversized insets and splitters
    // produce — count, so callers can test the result of either.
    bool isEmpty() const;

    bool intersects(const Rect& other) const;

    // The overlap, or an empty rect when there is none. Nesting clip regions is
    // the reason this exists: the GPU has exactly one scissor rect and no stack,
    // so a child's clip has to be intersected with its parent's on the way down
    // rather than replacing it.
    Rect intersection(const Rect& other) const;

    Rect inset(float amount) const;
    Rect inset(float horizontal, float vertical) const;
    Rect inset(const Insets& insets) const;

    Rect withX(float newX) const;
    Rect withY(float newY) const;
    Rect withWidth(float newW) const;
    Rect withHeight(float newH) const;
    Rect withPosition(float newX, float newY) const;
    Rect withSize(float newW, float newH) const;

    Rect fromLeft(float width, float margin = 0.f) const;
    Rect fromRight(float width, float margin = 0.f) const;
    Rect fromTop(float height, float margin = 0.f) const;
    Rect fromBottom(float height, float margin = 0.f) const;

    Rect removeFromLeft(float amount);
    Rect removeFromRight(float amount);
    Rect removeFromTop(float amount);
    Rect removeFromBottom(float amount);

    Point center() const;

    // Edge coordinates. `top` is the smaller y and `bottom` the larger, so
    // top <= bottom for any rect with a non-negative height.
    float left() const;
    float right() const;
    float top() const;
    float bottom() const;

    float x = 0.f;
    float y = 0.f;
    float w = 0.f;
    float h = 0.f;
};

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
