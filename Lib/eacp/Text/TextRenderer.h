#pragma once

#include "GlyphAtlas.h"
#include "GlyphRenderer.h"

#include <cstdint>
#include <string_view>

namespace eacp::Text
{
// One glyph a layout placed, kept by a caller that lays a string out once
// rather than per frame. See layoutInto, drawGlyph, and generation(), which
// says when a kept placement has stopped being valid.
struct PlacedGlyph
{
    // In the caller's own points.
    Graphics::Rect destination;

    // In atlas texels, which survive the atlas growing. Only a clear
    // invalidates them, and generation() is what says so.
    Graphics::Rect source;

    bool colored = false;
};

// Draws strings: an atlas, a glyph renderer and the layout loop that places
// each glyph by its bearings, agreeing about the device scale so glyphs land
// 1:1 on the panel. Re-rasterizes when the backing scale changes.
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

    // The face used by the calls that name none. Sizes coexist in one atlas, so
    // changing it costs only the glyphs of the new face.
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

    // `baselineLeft` is the pen: x at the string's left edge, y on the
    // baseline. Returns the advance, so calls chain into a line of runs. Faces
    // share the atlas and the batch, so the Font overload costs nothing extra.
    float draw(std::string_view text,
               Graphics::Point baselineLeft,
               const Graphics::Color& color,
               FontStyle style = FontStyle::Regular);

    float draw(std::string_view text,
               Graphics::Point baselineLeft,
               const Graphics::Color& color,
               const Font& font);

    // Appends each drawable glyph to `into` instead of queueing it, and returns
    // the advance the way draw() does -- one walk for both answers.
    float layoutInto(Vector<PlacedGlyph>& into,
                     std::string_view text,
                     Graphics::Point baselineLeft,
                     const Font& font);

    // Queues a glyph an earlier layoutInto placed. The colour is given here so
    // one recorded run can be drawn in any colour.
    void drawGlyph(const PlacedGlyph& glyph, const Graphics::Color& color);

    // Ticks when the atlas clears, leaving every PlacedGlyph handed out before
    // it pointing at somebody else's texels. Compare it before reusing one.
    std::uint32_t generation() const;

    // The advance `text` would take, without drawing it.
    float measure(std::string_view text, FontStyle style = FontStyle::Regular);
    float measure(std::string_view text, const Font& font);

    void begin();

    // Submits everything queued since begin(), uploading whatever the atlas
    // rasterized on the way, so it must run before the pass samples it.
    void flush(GPU::RenderPass& pass);

private:
    void rebuildIfNeeded();

    // Builds the atlas first if the viewport has not yet been seen.
    int faceFor(const Font& font);

    // One loop rather than two, so measure() and draw() cannot drift. Glyphs go
    // to `into` when there is one, and the renderer's own queue when not.
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
