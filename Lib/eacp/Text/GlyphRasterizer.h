#pragma once

#include "Font.h"
#include "GlyphBitmap.h"

#include <eacp/Core/Utils/Pimpl.h>

namespace eacp::Text
{
// Where GlyphAtlas gets its pixels, an interface so the atlas can be driven by
// a stub rather than by whatever fonts a machine has installed.
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

// The whole platform surface of this module: CoreText on Apple, DirectWrite on
// Windows. Grayscale on both and never subpixel/LCD, which would bake one text
// colour into the cache and cannot coexist with a transparent window.
class GlyphRasterizer final : public GlyphSource
{
public:
    explicit GlyphRasterizer(const FontRequest& request);
    ~GlyphRasterizer() override;

    GlyphRasterizer(const GlyphRasterizer&) = delete;
    GlyphRasterizer& operator=(const GlyphRasterizer&) = delete;

    // False when the family could not be resolved and no substitute was found.
    bool isValid() const;

    // In device pixels, for the requested style, which can differ between faces
    // of one family.
    FontMetrics metrics(FontStyle style) const override;

    float scale() const override;

    // Falls back to another face when this one has no glyph, so CJK and emoji
    // still render from a Latin family, reporting Color format for a colour
    // font. The bitmap is invalid when nothing can draw the codepoint.
    GlyphBitmap rasterize(char32_t codepoint, FontStyle style) const override;

    const FontRequest& request() const;

private:
    struct Native;
    Pimpl<Native> impl;
};

// Registers an in-memory font with the platform, so a FontRequest naming it
// resolves without the file being installed. False when the data is not a
// usable font; registering the same face twice is harmless.
bool registerMemoryFont(const void* data, std::size_t size);
} // namespace eacp::Text
