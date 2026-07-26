#include "TextRenderer.h"

#include <utility>

namespace eacp::Text
{
namespace
{
// Decodes one UTF-8 sequence starting at `index`, advancing it past what was
// consumed. Malformed bytes yield U+FFFD and advance by one, so a bad byte
// costs one replacement glyph rather than desynchronising the rest of the line.
char32_t nextCodepoint(std::string_view text, std::size_t& index)
{
    auto lead = (unsigned char) text[index];

    auto continuationBytes = lead < 0x80   ? 0
                             : lead < 0xC0 ? -1
                             : lead < 0xE0 ? 1
                             : lead < 0xF0 ? 2
                             : lead < 0xF8 ? 3
                                           : -1;

    if (continuationBytes < 0 || index + continuationBytes >= text.size())
    {
        ++index;
        return 0xFFFD;
    }

    constexpr char32_t leadMask[] = {0x7F, 0x1F, 0x0F, 0x07};
    auto codepoint = (char32_t) (lead & leadMask[continuationBytes]);

    for (auto i = 1; i <= continuationBytes; ++i)
    {
        auto byte = (unsigned char) text[index + i];

        if ((byte & 0xC0) != 0x80)
        {
            ++index;
            return 0xFFFD;
        }

        codepoint = (codepoint << 6) | (byte & 0x3F);
    }

    index += continuationBytes + 1;
    return codepoint;
}
} // namespace

TextRenderer::TextRenderer(float pointSizeToUse, std::string familyToUse)
    : family(std::move(familyToUse))
    , fontPointSize(pointSizeToUse)
{
}

TextRenderer::~TextRenderer() = default;

void TextRenderer::setViewport(Graphics::Point size, float scale)
{
    viewportSize = size;
    deviceScale = scale > 0.0f ? scale : 1.0f;
}

void TextRenderer::setPointSize(float pointSizeToUse)
{
    fontPointSize = pointSizeToUse;
}

void TextRenderer::rebuildIfNeeded()
{
    if (atlas != nullptr && builtAtScale == deviceScale
        && builtAtPointSize == fontPointSize)
        return;

    auto request = FontRequest {};
    request.family = family;
    request.pointSize = fontPointSize;
    request.scale = deviceScale;

    // Replacing rather than resizing: every cached slot was rasterized for the
    // old scale, and keeping them would mix sizes in one atlas.
    atlas = makeOwned<GlyphAtlas>(makeOwned<GlyphRasterizer>(request));
    builtAtScale = deviceScale;
    builtAtPointSize = fontPointSize;
}

float TextRenderer::lineHeight() const
{
    return atlas != nullptr ? atlas->metrics().lineHeight() : fontPointSize;
}

float TextRenderer::ascent() const
{
    return atlas != nullptr ? atlas->metrics().ascent : fontPointSize;
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
                           FontStyle style,
                           bool emit)
{
    rebuildIfNeeded();

    auto advance = 0.0f;
    auto index = std::size_t {0};

    while (index < text.size())
    {
        auto glyph = atlas->glyph(nextCodepoint(text, index), style);

        if (!glyph.valid)
            continue;

        if (emit && !glyph.empty)
        {
            // The slot's src is in atlas texels but its offset and advance are
            // already in points, so only the size needs dividing by the scale.
            auto destination = Graphics::Rect {pen.x + advance + glyph.offset.x,
                                               pen.y + glyph.offset.y,
                                               glyph.src.w / builtAtScale,
                                               glyph.src.h / builtAtScale};

            glyphs->add(
                destination, glyph.src, color, glyph.format == GlyphFormat::Color);
        }

        advance += glyph.advance;
    }

    return advance;
}

float TextRenderer::draw(std::string_view text,
                         Graphics::Point baselineLeft,
                         const Graphics::Color& color,
                         FontStyle style)
{
    if (!glyphs.has_value())
        begin();

    return layout(text, baselineLeft, color, style, true);
}

float TextRenderer::measure(std::string_view text, FontStyle style)
{
    return layout(text, {}, Graphics::Color::white(), style, false);
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
