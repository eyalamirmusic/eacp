#pragma once

#include "Common.h"

#include <cstdint>
#include <string>

namespace eacp::Text
{
// The four faces a code editor or terminal actually switches between mid-line.
// Deliberately not a general weight axis: the atlas keys on this, and a
// two-bit style keeps one glyph to one cache entry per face.
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
// so asking for a literal one gets you a substitute on the other platform —
// a proportional substitute, which quietly loses the property most callers of a
// monospace family wanted in the first place.
constexpr const char* defaultMonospaceFamily()
{
#if defined(_WIN32)
    return "Consolas";
#else
    return "Menlo";
#endif
}

// What to rasterize with.
//
// pointSize is in logical points and scale is device pixels per point, so the
// rasterizer works at pointSize * scale and everything the caller sees comes
// back in points. That split is why glyphs land 1:1 on a Retina panel instead
// of being magnified from a 1x bitmap.
struct FontRequest
{
    std::string family = defaultMonospaceFamily();
    float pointSize = 13.f;
    float scale = 1.f;

    float pixelSize() const { return pointSize * scale; }
};

// Face metrics, in device pixels — the same space GlyphBitmap reports its
// bearings in. GlyphAtlas divides by the scale before handing anything out.
struct FontMetrics
{
    float ascent = 0.f;
    float descent = 0.f;
    float leading = 0.f;

    // The advance of 'M'. A monospace grid steps by this; a proportional face
    // reports it only as a reasonable column guess.
    float advance = 0.f;

    float lineHeight() const { return ascent + descent + leading; }
};
} // namespace eacp::Text
