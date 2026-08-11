#pragma once

#include "Common.h"

#include <cstdint>
#include <vector>

namespace eacp::Text
{
// Mask is one byte of coverage per pixel, tinted at draw time. Color is four
// bytes with straight (un-premultiplied) alpha, for colour fonts, which carry
// their own colour and must not be tinted.
enum class GlyphFormat : std::uint8_t
{
    Mask,
    Color
};

constexpr int bytesPerPixel(GlyphFormat format)
{
    return format == GlyphFormat::Color ? 4 : 1;
}

// Pixels plus the geometry needed to place them. Every measurement is in device
// pixels, matching FontMetrics.
struct GlyphBitmap
{
    std::vector<std::uint8_t> pixels;

    int width = 0;
    int height = 0;
    GlyphFormat format = GlyphFormat::Mask;

    // Left edge of the bitmap relative to the pen position.
    float bearingX = 0.f;

    // Top edge of the bitmap relative to the baseline, positive upwards — so a
    // descender's top is still positive and its bottom falls below the line.
    float bearingY = 0.f;

    float advance = 0.f;

    // False when nothing, face or fallback, had a glyph for the codepoint. A
    // valid bitmap may still be empty: a space advances and draws nothing.
    bool valid = false;

    bool isEmpty() const { return width <= 0 || height <= 0; }

    std::size_t bytesPerRow() const
    {
        return static_cast<std::size_t>(width) * bytesPerPixel(format);
    }
};
} // namespace eacp::Text
