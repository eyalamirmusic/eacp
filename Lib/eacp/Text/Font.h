#pragma once

#include "Common.h"

#include <cstdint>
#include <string>

namespace eacp::Text
{
// Not a general weight axis: the atlas keys on this, and two bits keep one
// glyph to one cache entry per face.
enum class FontStyle : std::uint8_t
{
    Regular = 0,
    Bold = 1,
    Italic = 2,
    BoldItalic = 3
};

constexpr FontStyle toFontStyle(bool bold, bool italic)
{
    return static_cast<FontStyle>((bold ? 1 : 0) | (italic ? 2 : 0));
}

constexpr bool isBold(FontStyle style)
{
    return (static_cast<std::uint8_t>(style) & 1) != 0;
}

constexpr bool isItalic(FontStyle style)
{
    return (static_cast<std::uint8_t>(style) & 2) != 0;
}

// The platform's stock fixed-pitch face. No family name ships on both systems,
// and a literal one substitutes to a proportional face on the other.
constexpr const char* defaultMonospaceFamily()
{
#if defined(_WIN32)
    return "Consolas";
#else
    return "Menlo";
#endif
}

// What to rasterize with: pointSize in logical points, scale in device pixels
// per point, so the rasterizer works at pointSize * scale and the caller still
// sees points.
struct FontRequest
{
    std::string family = defaultMonospaceFamily();
    float pointSize = 13.f;
    float scale = 1.f;

    float pixelSize() const { return pointSize * scale; }
};

// A face named completely, in points. Unlike FontRequest it carries no device
// scale: nothing above the atlas rasterizes, so nothing above it needs one.
struct Font
{
    std::string family = defaultMonospaceFamily();
    float pointSize = 13.f;
    FontStyle style = FontStyle::Regular;
};

// How close two sizes have to be to share a face. A tolerance and not an exact
// match, or a document resized by a drag would take a face and an atlas full of
// glyphs per frame of it.
constexpr float faceSizeTolerance = 0.01f;

inline bool sameFace(const Font& a, const Font& b)
{
    return a.family == b.family
           && (a.pointSize - b.pointSize) * (a.pointSize - b.pointSize)
                  < faceSizeTolerance * faceSizeTolerance;
}

// Face metrics, in device pixels — the same space GlyphBitmap reports its
// bearings in. GlyphAtlas divides by the scale before handing anything out.
struct FontMetrics
{
    float ascent = 0.f;
    float descent = 0.f;
    float leading = 0.f;

    // The advance of 'M': a monospace grid steps by it, a proportional face
    // reports it only as a column guess.
    float advance = 0.f;

    float lineHeight() const { return ascent + descent + leading; }
};
} // namespace eacp::Text
