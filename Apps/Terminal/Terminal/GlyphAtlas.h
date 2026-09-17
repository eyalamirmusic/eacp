#pragma once

#include <eacp/Core/Utils/Pimpl.h>
#include <eacp/GPU/GPU.h>

#include <string>

namespace term
{
struct GlyphSlot
{
    eacp::Graphics::Rect src;
    bool colored = false;
    bool valid = false;
};

// Rasterizes monospace glyphs on demand (CoreText on macOS) into an RGBA
// atlas, uploaded lazily as one GPU texture. Monochrome glyphs are stored as
// white + coverage alpha so the renderer tints them per cell; color glyphs
// (emoji) are stored as-is and drawn untinted. Cell metrics are in points;
// the atlas itself is rasterized at the display's backing scale.
class GlyphAtlas
{
public:
    // scale <= 0 uses the main display's backing scale.
    GlyphAtlas(const std::string& fontName, float pointSize, float scale = 0);
    ~GlyphAtlas();

    float cellWidth() const;
    float cellHeight() const;
    float baseline() const;
    float fontSize() const;

    const GlyphSlot& glyph(char32_t cp, bool bold, bool italic);
    eacp::GPU::Texture& texture();

private:
    struct Impl;
    eacp::Pimpl<Impl> impl;
};
} // namespace term
