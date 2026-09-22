#pragma once

#include <eacp/Text/Text.h>

#include <optional>
#include <string_view>

// The labels, out of eacp-text's glyph atlas — the same path Apps/GPU/GlyphAtlas
// takes, wrapped so both views here share it: rasterize on demand, cache, upload
// only what changed, then draw every glyph of a frame as one instanced batch.
//
// The two-pass shape is the atlas's, not this wrapper's: every glyph a frame
// needs has to be requested before the first draw and committed once, because
// uploading mid-pass would mutate a texture the earlier draws already bound.
// So add() queues a label and flush() shapes the lot, commits, and only then
// places anything.
//
// Measuring goes through the same shaping, which is what lets the popup size
// itself from its content rather than from a guess: the width of a menu is the
// widest label in it, in the face it will actually be drawn in.

namespace PopupMenu
{
constexpr const char* menuFontFamily()
{
#if defined(_WIN32)
    return "Segoe UI";
#elif defined(__linux__)
    return "DejaVu Sans";
#else
    return "Helvetica Neue";
#endif
}

class MenuText
{
public:
    // Built once the view knows its display's scale, and rebuilt at the new one
    // when the window moves to a different display: glyphs cached for the old
    // scale are the wrong size.
    void setScale(float scale)
    {
        if (scale <= 0.f || scale == builtAtScale)
            return;

        // Recorded before the build rather than after it, so a machine with no
        // font resolves nothing once rather than once a frame.
        builtAtScale = scale;

        if (atlas)
        {
            atlas->setScale(scale);
            return;
        }

        auto request = eacp::Text::FontRequest {};
        request.family = menuFontFamily();
        request.pointSize = pointSize;
        request.scale = scale;

        if (!eacp::Text::GlyphRasterizer {request}.isValid())
            return;

        atlas = eacp::makeOwned<eacp::Text::GlyphAtlas>(
            eacp::Text::rasterizerFaceFactory(), request, 256, 2048);

        glyphs.emplace();
    }

    // False where fontconfig (or the platform's own font table) resolved
    // nothing, which is a machine with no fonts installed rather than a
    // failure: the example still runs, with no labels on it.
    bool isValid() const { return atlas != nullptr; }

    float lineHeight() const { return atlas ? atlas->metrics().lineHeight() : 0.f; }
    float ascent() const { return atlas ? atlas->metrics().ascent : 0.f; }
    float descent() const { return atlas ? atlas->metrics().descent : 0.f; }

    float measure(std::string_view text)
    {
        if (!atlas)
            return 0.f;

        return atlas->shape(text, variant()).advance;
    }

    void begin(eacp::Graphics::Point size)
    {
        labels.clear();

        if (glyphs)
            glyphs->setViewportSize(size);
    }

    // `pen` is the left end of the baseline. The text is not copied: every
    // label queued in a frame is owned by the view that queued it and outlives
    // the flush.
    void add(std::string_view text,
             eacp::Graphics::Point pen,
             const eacp::Graphics::Color& color)
    {
        if (atlas)
            labels.add({text, pen, color});
    }

    void flush(eacp::GPU::RenderPass& pass)
    {
        if (!atlas || !glyphs || labels.empty())
            return;

        for (const auto& label: labels)
            atlas->shape(label.text, variant());

        atlas->commit();
        glyphs->begin();

        for (const auto& label: labels)
            place(label);

        glyphs->flush(pass, *atlas);
        labels.clear();
    }

private:
    struct Label
    {
        std::string_view text;
        eacp::Graphics::Point pen;
        eacp::Graphics::Color color;
    };

    static eacp::Text::FontVariant variant() { return {400, false}; }

    void place(const Label& label)
    {
        const auto shaped = atlas->shape(label.text, variant());

        for (const auto& placed: shaped.glyphs)
        {
            const auto& glyph = placed.slot;

            if (!glyph.valid || glyph.empty)
                continue;

            // The glyph's pen and the slot's offset are both measured from the
            // label's pen and baseline, so this is the destination directly.
            const auto destination =
                eacp::Graphics::Rect {label.pen.x + placed.pen.x + glyph.offset.x,
                                      label.pen.y + placed.pen.y + glyph.offset.y,
                                      glyph.src.w / builtAtScale,
                                      glyph.src.h / builtAtScale};

            glyphs->add(destination,
                        glyph.src,
                        label.color,
                        glyph.format == eacp::Text::GlyphFormat::Color);
        }
    }

    static constexpr auto pointSize = 13.f;

    eacp::OwningPointer<eacp::Text::GlyphAtlas> atlas;
    std::optional<eacp::Text::GlyphRenderer> glyphs;
    eacp::Vector<Label> labels;
    float builtAtScale = 0.f;
};
} // namespace PopupMenu
