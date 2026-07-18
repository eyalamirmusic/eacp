#include "GlyphRasterizer.h"

// Windows rasterizer — NOT IMPLEMENTED YET.
//
// eacp-text was built macOS-first, with the platform seam kept narrow
// deliberately: everything above GlyphSource (packing, caching, growth, upload,
// metric conversion) is portable and already tested, so the DirectWrite backend
// is the only piece outstanding.
//
// This file exists so the Windows build stays green rather than failing to
// configure on a missing source. It reports isValid() == false, which every
// caller already has to handle — a font family that cannot be resolved takes
// the same path — so nothing crashes; text simply does not draw.
//
// Implementing it means porting the DirectWrite path CowTerm already proved
// (Terminal/GlyphAtlas-Windows.cpp), with three points worth carrying over:
//
//  - Build an IDWriteTextLayout per glyph rather than a raw glyph run: the
//    layout is what resolves system font fallback for emoji, CJK and symbols.
//  - Set D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE. ClearType would bake subpixel
//    colour into the mask, and the atlas is tinted at draw time.
//  - DirectWrite reports no colour-glyph trait on that path, so colour has to
//    be detected from the pixels — white text premultiplied means every channel
//    equals alpha, and anything else came from a colour font.
//
// Unlike CowTerm's version this one must also report bearings and advance, and
// emit a one-byte-per-pixel mask rather than white-plus-alpha RGBA.

namespace eacp::Text
{
struct GlyphRasterizer::Native
{
    explicit Native(const FontRequest& requestToUse)
        : request(requestToUse)
    {
    }

    FontRequest request;
};

GlyphRasterizer::GlyphRasterizer(const FontRequest& request)
    : impl(request)
{
}

GlyphRasterizer::~GlyphRasterizer() = default;

bool GlyphRasterizer::isValid() const
{
    return false;
}

FontMetrics GlyphRasterizer::metrics(FontStyle) const
{
    return {};
}

float GlyphRasterizer::scale() const
{
    return impl->request.scale;
}

GlyphBitmap GlyphRasterizer::rasterize(char32_t, FontStyle) const
{
    return {};
}

const FontRequest& GlyphRasterizer::request() const
{
    return impl->request;
}

bool registerMemoryFont(const void*, std::size_t)
{
    return false;
}
} // namespace eacp::Text
