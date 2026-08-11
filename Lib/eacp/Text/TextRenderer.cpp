#include "TextRenderer.h"

#include <utility>

namespace eacp::Text
{
namespace
{
// Advances `index` past what it consumed. A malformed byte yields U+FFFD and
// advances one, costing a replacement glyph rather than the rest of the line.
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
        // Only the scale rebuilds anything: a changed size is just a face the
        // next request adds beside the others.
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
    // In its own statement: resolving the face is what builds the atlas, so
    // reading the pointer in one expression would read it before there is one.
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

    auto advance = 0.0f;
    auto index = std::size_t {0};

    while (index < text.size())
    {
        auto glyph = atlas->glyph(nextCodepoint(text, index), font.style, face);

        if (!glyph.valid)
            continue;

        if (emit && !glyph.empty)
        {
            // src is in atlas texels while offset and advance are in points, so
            // only the size needs dividing by the scale.
            auto placed = PlacedGlyph {{pen.x + advance + glyph.offset.x,
                                        pen.y + glyph.offset.y,
                                        glyph.src.w / builtAtScale,
                                        glyph.src.h / builtAtScale},
                                       glyph.src,
                                       glyph.format == GlyphFormat::Color};

            if (into != nullptr)
                into->add(placed);
            else
                glyphs->add(
                    placed.destination, placed.source, color, placed.colored);
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
    // No begin(), unlike draw(): nothing is queued, so a recorded layout is
    // legal outside a frame entirely, with no pass open.
    return layout(text, baselineLeft, Graphics::Color::white(), font, true, &into);
}

void TextRenderer::drawGlyph(const PlacedGlyph& glyph, const Graphics::Color& color)
{
    if (!glyphs.has_value())
        begin();

    glyphs->add(glyph.destination, glyph.source, color, glyph.colored);
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

    // Everything the frame asked for is rasterized by now, so upload it before
    // the pass samples the atlas.
    atlas->commit();
    glyphs->flush(pass, *atlas);
}
} // namespace eacp::Text
