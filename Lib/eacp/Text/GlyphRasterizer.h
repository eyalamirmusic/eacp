#pragma once

#include "Font.h"
#include "GlyphBitmap.h"

#include <eacp/Core/Utils/Pimpl.h>

namespace eacp::Text
{
// Where GlyphAtlas gets its pixels. GlyphRasterizer is the real implementation;
// the indirection exists so the atlas — packing, growth, upload, eviction, all
// of which is portable logic worth testing hard — can be driven by a stub
// instead of by whatever fonts a given machine happens to have installed.
class GlyphSource
{
public:
    virtual ~GlyphSource() = default;

    virtual FontMetrics metrics(FontStyle style) const = 0;
    virtual GlyphBitmap rasterize(char32_t codepoint, FontStyle style) const = 0;

    // Device pixels per point, so the atlas can convert pixel-space bitmap
    // metrics into the logical points its callers lay out in.
    virtual float scale() const = 0;
};

// The whole platform surface of this module: turn a codepoint into pixels and
// metrics. CoreText on Apple, DirectWrite on Windows.
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

    // In device pixels, for the requested style. Faces in a family can differ:
    // a bold face is often slightly wider than its regular sibling.
    FontMetrics metrics(FontStyle style) const override;

    float scale() const override;

    // Rasterizes one codepoint. Falls back to another face when the requested
    // one has no glyph, so CJK and emoji still render from a Latin family; the
    // returned bitmap reports Color format when the fallback was a colour font.
    //
    // Returns an invalid bitmap when nothing can draw the codepoint.
    GlyphBitmap rasterize(char32_t codepoint, FontStyle style) const override;

    const FontRequest& request() const;

private:
    struct Native;
    Pimpl<Native> impl;
};

// Registers a font held in memory (an embedded .ttf) with the platform's font
// system, so a FontRequest naming it resolves without the file ever being
// installed or written to disk. Returns false when the data is not a usable
// font. Registering the same face twice is harmless.
bool registerMemoryFont(const void* data, std::size_t size);
} // namespace eacp::Text
