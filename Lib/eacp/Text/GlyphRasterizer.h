#pragma once

#include "Font.h"
#include "GlyphBitmap.h"

#include <eacp/Core/Utils/Pimpl.h>

#include <optional>
#include <string>
#include <string_view>

namespace eacp::Text
{
// A glyph as a shaper names it: its index in a font's own table, and which
// font that is. Font 0 is the face itself; a run the platform had to reach
// into another face for — CJK in a Latin family, an emoji — names that face
// by the number the source gave it when it first met it, so the same id in
// two fonts is two glyphs.
struct GlyphKey
{
    std::uint32_t glyph = 0;
    int font = 0;

    bool operator==(const GlyphKey&) const = default;
};

// One glyph a shaper placed. Positions are in device pixels from the run's
// pen and baseline, x along the line and y upwards, the way GlyphBitmap
// reports its bearings; `cluster` is the byte offset in the shaped text of
// the first character the glyph came from, which is how a caller maps a
// glyph back to the text — a ligature carries the offset of its first
// letter, and a mark that of its base.
struct ShapedGlyph
{
    GlyphKey key;
    float x = 0.f;
    float y = 0.f;
    int cluster = 0;
};

// A string shaped in a face: the glyphs, and how far the pen moved. Kerning,
// ligatures, mark placement and fallback are all inside, which is the whole
// difference from walking codepoints — "AV" is two glyphs closer together
// than their advances, "fi" is one glyph in a face that has it.
struct ShapedRun
{
    Vector<ShapedGlyph> glyphs;
    float advance = 0.f;
};

// Where GlyphAtlas gets its pixels. GlyphRasterizer is the real implementation;
// the indirection exists so the atlas — packing, growth, upload, eviction, all
// of which is portable logic worth testing hard — can be driven by a stub
// instead of by whatever fonts a given machine happens to have installed.
class GlyphSource
{
public:
    virtual ~GlyphSource() = default;

    virtual FontMetrics metrics(const FontVariant& variant) const = 0;

    // Turns a string into placed glyphs. The platform's shaper, so the glyph
    // ids it returns are the ones rasterize() understands.
    virtual ShapedRun shape(std::string_view text,
                            const FontVariant& variant) const = 0;

    // Rasterizes one glyph shape() named.
    virtual GlyphBitmap rasterize(GlyphKey glyph,
                                  const FontVariant& variant) const = 0;

    // Device pixels per point, so the atlas can convert pixel-space bitmap
    // metrics into the logical points its callers lay out in.
    virtual float scale() const = 0;
};

// The whole platform surface of this module: turn a string into glyphs, and
// a glyph into pixels and metrics. CoreText on Apple, DirectWrite on Windows.
//
// Everything else — packing, caching, growth, GPU upload — is portable and sits
// on top of this interface, which is what lets the atlas be tested against a
// fake rasterizer rather than against whatever fonts a machine happens to have.
//
// Rasterization is grayscale on both platforms, never subpixel/LCD: the atlas
// stores coverage and the colour arrives at draw time, so subpixel antialiasing
// would bake one particular text colour into the cache. It also cannot coexist
// with a transparent window background, and macOS dropped it in Mojave.
class GlyphRasterizer final : public GlyphSource
{
public:
    explicit GlyphRasterizer(const FontRequest& request);
    ~GlyphRasterizer() override;

    GlyphRasterizer(const GlyphRasterizer&) = delete;
    GlyphRasterizer& operator=(const GlyphRasterizer&) = delete;

    // False when the family could not be resolved and no substitute was found.
    bool isValid() const;

    // The family the platform actually resolved the request to: the one asked
    // for when it has it, and the substitute it chose when it does not - which
    // is how a caller holding a list of families finds the first one the
    // platform has, since both platforms draw *something* for any name.
    std::string resolvedFamily() const;

    // In device pixels, for the requested variant. Faces in a family can
    // differ: a bold face is often slightly wider than its regular sibling.
    FontMetrics metrics(const FontVariant& variant) const override;
    FontMetrics metrics(FontStyle style) const { return metrics(variantOf(style)); }

    float scale() const override;

    // Shapes through the platform — CTLine on Apple, IDWriteTextLayout on
    // Windows — in the face nearest the variant the family has, falling
    // back to other faces for what it lacks so CJK and emoji still shape
    // from a Latin family; each fallback face is numbered as it is met.
    ShapedRun shape(std::string_view text,
                    const FontVariant& variant) const override;

    // Rasterizes one glyph a shape() named; the bitmap reports Color format
    // when the glyph's font is a colour font. Invalid when no font has it.
    GlyphBitmap rasterize(GlyphKey glyph, const FontVariant& variant) const override;

    // One codepoint, shaped on its own and rasterized: what a caller walking
    // a string by codepoint asks for, and what a test of a single glyph wants.
    GlyphBitmap rasterize(char32_t codepoint, FontStyle style) const;

    const FontRequest& request() const;

private:
    struct Native;
    Pimpl<Native> impl;
};

// What registering a font made visible: the family the platform filed the
// face under, which is the name a FontRequest resolves - and not necessarily
// the one the caller knew it by, since a page's @font-face names a family of
// its own choosing - and the face's PostScript name, which names that face
// alone and which both platforms resolve too.
struct RegisteredFont
{
    std::string family;
    std::string postScriptName;
};

// Registers a font held in memory (an embedded .ttf, a page's web font) with
// the platform's font system, so a FontRequest naming it resolves without the
// file ever being installed or written to disk; the bytes are copied, so the
// caller's buffer need not outlive the call. Nothing when the data is not a
// usable font. Registering the same face twice reports it as it was, since a
// page reloaded registers its fonts again.
std::optional<RegisteredFont> registerMemoryFont(const void* data, std::size_t size);
} // namespace eacp::Text
