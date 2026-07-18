#pragma once

#include "Common.h"

#include <cstdint>
#include <vector>

namespace eacp::Text
{
// One rasterized glyph, before it goes into an atlas.
//
// Two pixel formats, because glyphs come in two kinds and they cannot share a
// texture format usefully:
//
//  - Mask: one byte of coverage per pixel. Almost every glyph. Stored as
//    coverage alone rather than white-plus-alpha so the atlas can be R8Unorm,
//    a quarter the memory of RGBA8, with the colour supplied per cell at draw
//    time.
//  - Color: four bytes per pixel, straight (un-premultiplied) alpha. Emoji and
//    other colour fonts, which carry their own colour and must not be tinted.
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
//
// The bearings are what CowTerm's atlas lacked, and the reason it could only
// ever draw a fixed monospace grid: without them a glyph can only be centred in
// a cell, so ligatures, proportional text and kerned pairs are all out of
// reach.
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

    // How far the pen advances after this glyph.
    float advance = 0.f;

    // False when the face has no glyph for the codepoint and no fallback
    // supplied one. An empty-but-valid bitmap is different: a space is valid
    // and advances the pen while rasterizing to nothing.
    bool valid = false;

    bool isEmpty() const { return width <= 0 || height <= 0; }

    std::size_t bytesPerRow() const
    {
        return static_cast<std::size_t>(width) * bytesPerPixel(format);
    }
};
} // namespace eacp::Text
