#include "TextRenderer.h"

#include <cmath>
#include <utility>

namespace eacp::Text
{
TextRenderer::TextRenderer(float pointSizeToUse, std::string familyToUse)
    : defaultFont {std::move(familyToUse), pointSizeToUse}
{
}

TextRenderer::~TextRenderer() = default;

void TextRenderer::setViewport(Graphics::Point size, float scale)
{
    viewportSize = size;
    deviceScale = scale > 0.0f ? scale : 1.0f;
}

void TextRenderer::setFont(const Font& font)
{
    defaultFont = font;
}

void TextRenderer::setPointSize(float pointSizeToUse)
{
    defaultFont.pointSize = pointSizeToUse;
}

void TextRenderer::rebuildIfNeeded()
{
    if (atlas == nullptr)
    {
        auto request = FontRequest {};
        request.family = defaultFont.family;
        request.pointSize = defaultFont.pointSize;
        request.scale = deviceScale;

        atlas = makeOwned<GlyphAtlas>(rasterizerFaceFactory(), request);
    }
    else
    {
        // Only the scale rebuilds anything now. A changed size is a face the
        // atlas did not have, which the next request adds beside the others
        // rather than in place of them.
        atlas->setScale(deviceScale);
    }

    builtAtScale = deviceScale;
}

int TextRenderer::faceFor(const Font& font)
{
    rebuildIfNeeded();

    return atlas->findOrAddFace(font);
}

float TextRenderer::lineHeight()
{
    return lineHeight(defaultFont);
}

float TextRenderer::lineHeight(const Font& font)
{
    // The face first, in its own statement: resolving it is what builds the
    // atlas, so reading the pointer in the same expression would read it before
    // there was one.
    const auto face = faceFor(font);

    return atlas->metrics(font.style, face).lineHeight();
}

float TextRenderer::ascent()
{
    return ascent(defaultFont);
}

float TextRenderer::ascent(const Font& font)
{
    const auto face = faceFor(font);

    return atlas->metrics(font.style, face).ascent;
}

void TextRenderer::begin()
{
    rebuildIfNeeded();

    if (!glyphs.has_value())
        glyphs.emplace();

    glyphs->setViewportSize(viewportSize);
    glyphs->begin();
}

float TextRenderer::layout(std::string_view text,
                           Graphics::Point pen,
                           const Graphics::Color& color,
                           const Font& font,
                           bool emit,
                           Vector<PlacedGlyph>* into)
{
    const auto face = faceFor(font);
    const auto shaped = atlas->shape(text, font.variant(), face);

    if (!emit)
        return shaped.advance;

    for (const auto& placed: shaped.glyphs)
    {
        const auto& glyph = placed.slot;

        if (!glyph.valid || glyph.empty)
            continue;

        const auto glyphPen =
            Graphics::Point {pen.x + placed.pen.x, pen.y + placed.pen.y};

        if (into == nullptr)
        {
            queueGlyph(glyphPen, placed.key, font.variant(), face, color);
            continue;
        }

        // Where the glyph lands give or take the snap, which is decided when
        // it is drawn and the pen is final. The slot's src is in atlas texels
        // but its offset and the pen are already in points, so only the size
        // needs dividing by the scale.
        auto destination = Graphics::Rect {glyphPen.x + glyph.offset.x,
                                           glyphPen.y + glyph.offset.y,
                                           glyph.src.w / builtAtScale,
                                           glyph.src.h / builtAtScale};

        into->add({glyphPen,
                   destination,
                   placed.key,
                   font.variant(),
                   face,
                   glyph.format == GlyphFormat::Color});
    }

    return shaped.advance;
}

namespace
{
// Whether text in this colour is light on a dark ground, for the rasterizer
// to thicken it the way the platform does. CoreGraphics steps up the
// thickening from mid grey and again from three quarters; one threshold
// between those two steps leaves either side within a few percent of it.
bool isLightText(const Graphics::Color& colour)
{
    const auto luma = 0.2126f * colour.r + 0.7152f * colour.g + 0.0722f * colour.b;

    return luma >= 0.7f;
}
} // namespace

SnappedPen snapPen(Graphics::Point pen, float scale)
{
    const auto x = pen.x * scale;
    auto pixel = std::floor(x);
    auto phase = (int) std::lround((x - pixel) * (float) subpixelPhases);

    if (phase == subpixelPhases)
    {
        pixel += 1.f;
        phase = 0;
    }

    return {(int) pixel, (int) std::lround(pen.y * scale), phase};
}

void TextRenderer::queueGlyph(Graphics::Point pen,
                              GlyphKey key,
                              const FontVariant& variant,
                              int face,
                              const Graphics::Color& color)
{
    const auto snapped = snapPen(pen, builtAtScale);
    const auto slot =
        atlas->glyph(key, variant, face, snapped.phase, isLightText(color));

    if (!slot.valid || slot.empty)
        return;

    // The pixel the pen snapped to, back in points, plus the slot's offset,
    // which is whole pixels over the same scale: every texel lands on a pixel
    // of its own, and the linear sampler has nothing to blend.
    const auto destination =
        Graphics::Rect {(float) snapped.x / builtAtScale + slot.offset.x,
                        (float) snapped.y / builtAtScale + slot.offset.y,
                        slot.src.w / builtAtScale,
                        slot.src.h / builtAtScale};

    glyphs->add(destination, slot.src, color, slot.format == GlyphFormat::Color);
}

float TextRenderer::draw(std::string_view text,
                         Graphics::Point baselineLeft,
                         const Graphics::Color& color,
                         FontStyle style)
{
    auto font = defaultFont;
    font.style = style;

    return draw(text, baselineLeft, color, font);
}

float TextRenderer::draw(std::string_view text,
                         Graphics::Point baselineLeft,
                         const Graphics::Color& color,
                         const Font& font)
{
    if (!glyphs.has_value())
        begin();

    return layout(text, baselineLeft, color, font, true);
}

float TextRenderer::layoutInto(Vector<PlacedGlyph>& into,
                               std::string_view text,
                               Graphics::Point baselineLeft,
                               const Font& font)
{
    // No begin() here, unlike draw(): nothing is being queued, so there is no
    // queue to have started. A recorded layout is legal outside a frame
    // entirely, which is what lets a component be painted with no pass open.
    return layout(text, baselineLeft, Graphics::Color::white(), font, true, &into);
}

void TextRenderer::drawGlyph(const PlacedGlyph& glyph, const Graphics::Color& color)
{
    if (!glyphs.has_value())
        begin();

    queueGlyph(glyph.pen, glyph.key, glyph.variant, glyph.face, color);
}

std::uint32_t TextRenderer::generation() const
{
    return atlas != nullptr ? atlas->generation() : 0;
}

float TextRenderer::measure(std::string_view text, FontStyle style)
{
    auto font = defaultFont;
    font.style = style;

    return measure(text, font);
}

float TextRenderer::measure(std::string_view text, const Font& font)
{
    return layout(text, {}, Graphics::Color::white(), font, false);
}

void TextRenderer::flush(GPU::RenderPass& pass)
{
    if (!glyphs.has_value() || atlas == nullptr)
        return;

    // Everything the frame asked for has been rasterized by now; upload it
    // before the pass samples the atlas.
    atlas->commit();
    glyphs->flush(pass, *atlas);
}
} // namespace eacp::Text
