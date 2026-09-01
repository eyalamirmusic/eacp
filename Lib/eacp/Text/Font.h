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

// What a family varies its faces by, on the axes CSS names: the weight on
// its 100 (thin) to 900 (black) scale, 400 regular and 700 bold, and the
// slant. The axis FontStyle folds to two bits, kept beside it rather than
// in it so a caller that has only ever wanted bold-or-not keeps its enum.
struct FontVariant
{
    int weight = 400;
    bool italic = false;

    bool operator==(const FontVariant&) const = default;
};

constexpr FontVariant variantOf(FontStyle style)
{
    return {isBold(style) ? 700 : 400, isItalic(style)};
}

constexpr FontStyle styleOf(const FontVariant& variant)
{
    return toFontStyle(variant.weight >= 600, variant.italic);
}

// The nearest of the nine CSS weights, so a face's own value keys the same
// way whichever of them it was asked for as.
constexpr int weightClass(int weight)
{
    const auto clamped = weight < 100 ? 100 : weight > 900 ? 900 : weight;

    return (clamped + 50) / 100 * 100;
}

// The platform's stock fixed-pitch face. No family name ships on both systems,
// so asking for a literal one gets you a substitute on the other platform —
// a proportional substitute, which quietly loses the property most callers of a
// monospace family wanted in the first place.
constexpr const char* defaultMonospaceFamily()
{
    if constexpr (Platform::isWindows())
        return "Consolas";

    return "Menlo";
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

// A face named completely: the family and size a FontStyle cannot vary, and the
// style it can.
//
// Distinct from FontRequest, which is what a *rasterizer* is built from and
// therefore carries the device scale. Nothing above the atlas rasterizes
// anything, so nothing above the atlas has to know the scale — a caller asks
// for 18pt Helvetica and gets 18 points on any display.
struct Font
{
    std::string family = defaultMonospaceFamily();
    float pointSize = 13.f;
    FontStyle style = FontStyle::Regular;

    // A weight on CSS's scale, 100 to 900, for a caller that has one. Zero
    // takes it from the style: 400, or 700 when the style is bold.
    int weight = 0;

    FontVariant variant() const
    {
        return {weight > 0 ? weightClass(weight) : (isBold(style) ? 700 : 400),
                isItalic(style)};
    }
};

// How close two sizes have to be to share a face. Matched with a tolerance
// rather than exactly because two sizes a hundredth of a point apart rasterize
// to the same glyphs: a document scaled by a drag asks for a slightly different
// size every frame, and an exact match would give it a face and an atlas full
// of glyphs per frame of the drag.
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

    // The advance of 'M'. A monospace grid steps by this; a proportional face
    // reports it only as a reasonable column guess.
    float advance = 0.f;

    float lineHeight() const { return ascent + descent + leading; }
};
} // namespace eacp::Text
