#pragma once

#include "GlyphAtlas.h"
#include "GlyphRenderer.h"

#include <cstdint>
#include <string_view>

namespace eacp::Text
{
// One glyph a layout placed: where it goes in the caller's own points, where it
// is in the atlas, and which of the two atlases that is.
//
// What a caller keeps when it wants the layout done once rather than once per
// frame. Laying a string out is a walk over its bytes with an atlas lookup per
// glyph, and a string that has not changed produces the same glyphs every time
// -- so a caller drawing an unchanged interface should be able to hold these and
// hand them straight back. See layoutInto and drawGlyph, and note generation():
// a slot kept across frames is only valid while the atlas has not been cleared
// underneath it.
struct PlacedGlyph
{
    Graphics::Rect destination;

    // In atlas texels, which survive the atlas growing -- placements only ever
    // extend right and down, and the size the shader divides by is read at the
    // draw. Only a clear invalidates these, and generation() is what says so.
    Graphics::Rect source;

    bool colored = false;
};

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

    // The face used by the calls that name none. Changing it costs nothing but
    // the glyphs of the new face: sizes coexist in one atlas, so the old one is
    // still there for whatever is still drawing in it.
    void setFont(const Font& font);
    const Font& getFont() const { return defaultFont; }

    void setPointSize(float pointSizeToUse);
    float pointSize() const { return defaultFont.pointSize; }

    // Distance between baselines, in logical points.
    float lineHeight();
    float lineHeight(const Font& font);

    // Baseline offset from the top of a line, for placing text against a box
    // rather than against a baseline.
    float ascent();
    float ascent(const Font& font);

    // Queue for drawing. `baselineLeft` is the pen: x at the string's left
    // edge, y on the baseline. Returns the advance, so successive calls can be
    // chained to build a line out of differently coloured runs.
    //
    // The overload taking a Font draws that face instead of the default one,
    // and costs nothing extra: faces share the atlas and the batch, so a
    // heading, a caption and a monospace log in one frame are one draw.
    float draw(std::string_view text,
               Graphics::Point baselineLeft,
               const Graphics::Color& color,
               FontStyle style = FontStyle::Regular);

    float draw(std::string_view text,
               Graphics::Point baselineLeft,
               const Graphics::Color& color,
               const Font& font);

    // Lays the string out and appends each drawable glyph to `into` instead of
    // queueing it, returning the advance the way draw() does.
    //
    // One walk for both answers, which is the point: a caller that records a
    // frame rather than issuing one needs the placements *and* the advance, and
    // measuring first and drawing afterwards walks the string twice for two
    // halves of the same loop.
    float layoutInto(Vector<PlacedGlyph>& into,
                     std::string_view text,
                     Graphics::Point baselineLeft,
                     const Font& font);

    // Queues a glyph an earlier layoutInto placed. The colour is given here
    // rather than held in the glyph, so one recorded run can be drawn in
    // whatever colour the caller is drawing in now.
    void drawGlyph(const PlacedGlyph& glyph, const Graphics::Color& color);

    // Ticks when the atlas is cleared, which is what makes every PlacedGlyph
    // handed out before it point at texels belonging to somebody else. A caller
    // holding placements across frames compares this and lays out again; one
    // that lays out every frame can ignore it.
    std::uint32_t generation() const;

    // The advance `text` would take, without drawing it.
    float measure(std::string_view text, FontStyle style = FontStyle::Regular);
    float measure(std::string_view text, const Font& font);

    void begin();

    // Submits everything queued since begin(). Uploads whatever the atlas
    // rasterized on the way, so it must run before the pass samples it.
    void flush(GPU::RenderPass& pass);

private:
    void rebuildIfNeeded();

    // The atlas's index for `font`, building the atlas first if the viewport
    // has not yet been seen.
    int faceFor(const Font& font);

    // Walks the string, optionally emitting each glyph, and returns the total
    // advance. One loop rather than two so measure() and draw() cannot drift.
    //
    // `into` is where the glyphs go when there is one: the caller's vector, for
    // a recorded layout, or the renderer's own queue when there is not.
    float layout(std::string_view text,
                 Graphics::Point pen,
                 const Graphics::Color& color,
                 const Font& font,
                 bool emit,
                 Vector<PlacedGlyph>* into = nullptr);

    Font defaultFont;
    float builtAtScale = 0.0f;

    Graphics::Point viewportSize {0.0f, 0.0f};
    float deviceScale = 1.0f;

    OwningPointer<GlyphAtlas> atlas;
    std::optional<GlyphRenderer> glyphs;
};
} // namespace eacp::Text
