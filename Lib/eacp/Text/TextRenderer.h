#pragma once

#include "GlyphAtlas.h"
#include "GlyphRenderer.h"

#include <string_view>

namespace eacp::Text
{
// Draws strings. An atlas, a glyph renderer and the layout loop that walks a
// string placing each glyph by its own bearings — the three things every
// consumer of this module otherwise assembles by hand, and which have to agree
// about the device scale to land 1:1 on the panel.
//
// Rebuilds itself when the font or the backing scale changes, so a window
// dragged between a Retina and a non-Retina display re-rasterizes rather than
// magnifying a 1x atlas.
//
// Usage, once per frame:
//
//     text.setViewport({bounds.w, bounds.h}, backingScale());
//     text.begin();
//     text.draw("hello", {10, 20}, Graphics::Color::white());
//     text.flush(pass);
class TextRenderer
{
public:
    explicit TextRenderer(float pointSizeToUse = 13.0f,
                          std::string familyToUse = defaultMonospaceFamily());
    ~TextRenderer();

    TextRenderer(const TextRenderer&) = delete;
    TextRenderer& operator=(const TextRenderer&) = delete;

    // The surface being drawn into and its device pixels per point. A changed
    // scale rebuilds the atlas; a changed size is free.
    void setViewport(Graphics::Point size, float scale);

    void setPointSize(float pointSizeToUse);
    float pointSize() const { return fontPointSize; }

    // Distance between baselines, in logical points.
    float lineHeight() const;

    // Baseline offset from the top of a line, for placing text against a box
    // rather than against a baseline.
    float ascent() const;

    // Queue for drawing. `baselineLeft` is the pen: x at the string's left
    // edge, y on the baseline. Returns the advance, so successive calls can be
    // chained to build a line out of differently coloured runs.
    float draw(std::string_view text,
               Graphics::Point baselineLeft,
               const Graphics::Color& color,
               FontStyle style = FontStyle::Regular);

    // The advance `text` would take, without drawing it.
    float measure(std::string_view text, FontStyle style = FontStyle::Regular);

    void begin();

    // Submits everything queued since begin(). Uploads whatever the atlas
    // rasterized on the way, so it must run before the pass samples it.
    void flush(GPU::RenderPass& pass);

private:
    void rebuildIfNeeded();

    // Walks the string, optionally emitting each glyph, and returns the total
    // advance. One loop rather than two so measure() and draw() cannot drift.
    float layout(std::string_view text,
                 Graphics::Point pen,
                 const Graphics::Color& color,
                 FontStyle style,
                 bool emit);

    std::string family;
    float fontPointSize = 13.0f;
    float builtAtScale = 0.0f;
    float builtAtPointSize = 0.0f;

    Graphics::Point viewportSize {0.0f, 0.0f};
    float deviceScale = 1.0f;

    OwningPointer<GlyphAtlas> atlas;
    std::optional<GlyphRenderer> glyphs;
};
} // namespace eacp::Text
