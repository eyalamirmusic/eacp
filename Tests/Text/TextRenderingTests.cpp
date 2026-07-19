#include "Common.h"

#include <eacp/Sprites/Sprites.h>

#include <optional>
#include <string>

// The whole path, end to end: rasterize -> pack -> upload -> sample -> pixels.
//
// The atlas tests above stop at the CPU boundary and the rasterizer tests stop
// at the bitmap. Everything between — that the sub-region upload puts coverage
// where the slot says it is, that the mask atlas samples as coverage rather
// than colour, that bearings and advances place glyphs correctly — only shows
// up once something is actually drawn. So these render off-screen through a
// GPUView and read the pixels back.
//
// Self-skips without a GPU device or a resolvable font.

using namespace nano;
using namespace eacp;
using namespace eacp::Text;

namespace
{
constexpr auto viewWidth = 320.f;
constexpr auto viewHeight = 64.f;

// Draws a string from the atlas, laying each glyph out by its own bearings.
struct TextView final : GPU::GPUView
{
    TextView()
    {
        setSampleCount(1);
        setBounds({0.f, 0.f, viewWidth, viewHeight});
    }

    bool build()
    {
        auto request = FontRequest {};
        request.family = defaultMonospaceFamily();
        request.pointSize = 24.f;
        request.scale = 1.f;

        auto rasterizer = makeOwned<GlyphRasterizer>(request);

        if (!rasterizer->isValid())
            return false;

        atlas = makeOwned<GlyphAtlas>(
            OwningPointer<GlyphSource> {std::move(rasterizer)}, 256, 1024);

        return true;
    }

    void render(GPU::Frame& frame) override
    {
        if (!sprites)
            sprites.emplace(Graphics::Point {viewWidth, viewHeight}, sampleCount());

        auto pass = frame.beginPass({{0.f, 0.f, 0.f, 1.f}});

        if (!atlas)
            return;

        // Rasterize everything first, then upload once, then draw — uploading
        // mid-pass would mutate a texture the earlier draws already bound.
        for (const auto character: text)
            atlas->glyph((char32_t) character, FontStyle::Regular);

        atlas->commit();
        sprites->begin(pass);

        const auto metrics = atlas->metrics();
        auto pen = penX;

        for (const auto character: text)
        {
            const auto glyph = atlas->glyph((char32_t) character, FontStyle::Regular);

            if (!glyph.valid)
                continue;

            if (!glyph.empty)
                sprites->drawTexture(glyph.format == GlyphFormat::Color
                                         ? atlas->colorTexture()
                                         : atlas->maskTexture(),
                                     glyph.src,
                                     {pen + glyph.offset.x,
                                      metrics.ascent + glyph.offset.y,
                                      glyph.src.w,
                                      glyph.src.h},
                                     Graphics::Color::white());

            pen += glyph.advance;
        }

        lastPen = pen;
    }

    std::string text;
    float penX = 4.f;
    float lastPen = 0.f;
    OwningPointer<GlyphAtlas> atlas;

    // Must outlive render(): a renderer built as a local there releases its
    // vertex buffer and pipeline while the command list recording the draws is
    // still waiting to be submitted, and on D3D12 the frame then draws nothing.
    std::optional<Sprites::SpriteRenderer> sprites;
};

int inkPixels(const Graphics::Image& image)
{
    auto total = 0;

    for (auto y = 0; y < image.height(); ++y)
        for (auto x = 0; x < image.width(); ++x)
            if (image.at(x, y).r > 0.35f)
                ++total;

    return total;
}

// Rightmost column carrying ink — how far the text actually reached.
int rightmostInk(const Graphics::Image& image)
{
    for (auto x = image.width() - 1; x >= 0; --x)
        for (auto y = 0; y < image.height(); ++y)
            if (image.at(x, y).r > 0.35f)
                return x;

    return -1;
}
} // namespace

// Text drawn through the atlas actually puts ink on the target. If the mask
// atlas were uploaded wrong, sampled wrong, or the slot rects were stale, this
// comes back black.
auto tDrawsInk = test("TextRendering/drawsGlyphsFromTheAtlas") = []
{
    if (!GPU::Device::shared().isValid())
        return;

    auto view = TextView {};

    if (!view.build())
        return;

    view.text = "Hello";

    auto image = view.renderToImage(1.f);

    check(image.isValid());
    check(inkPixels(image) > 20);
};

// Nothing drawn means nothing lit — the control that stops the test above from
// passing on a background that happens to be bright.
auto tEmptyTextDrawsNothing = test("TextRendering/emptyTextLeavesTheTargetClear") = []
{
    if (!GPU::Device::shared().isValid())
        return;

    auto view = TextView {};

    if (!view.build())
        return;

    view.text = "";

    auto image = view.renderToImage(1.f);

    check(image.isValid());
    check(inkPixels(image) == 0);
};

// Whitespace advances the pen without drawing, so two words reach further right
// than one while putting down the same amount of ink.
auto tSpacesAdvanceWithoutInk = test("TextRendering/spacesAdvanceThePenWithoutDrawing") = []
{
    if (!GPU::Device::shared().isValid())
        return;

    auto withoutSpaces = TextView {};
    auto withSpaces = TextView {};

    if (!withoutSpaces.build() || !withSpaces.build())
        return;

    withoutSpaces.text = "ab";
    withSpaces.text = "a   b";

    auto tight = withoutSpaces.renderToImage(1.f);
    auto spaced = withSpaces.renderToImage(1.f);

    check(tight.isValid() && spaced.isValid());

    // Same glyphs, so comparable ink; spaces push the second 'b' further right.
    check(rightmostInk(spaced) > rightmostInk(tight));

    const auto tightInk = inkPixels(tight);
    const auto spacedInk = inkPixels(spaced);

    check(spacedInk > tightInk / 2);
    check(spacedInk < tightInk * 2);
};

// Advances accumulate: more characters reach further right. This is what fails
// if every glyph is drawn at the pen without stepping it.
auto tAdvancesAccumulate = test("TextRendering/penAdvancesAcrossGlyphs") = []
{
    if (!GPU::Device::shared().isValid())
        return;

    auto shortText = TextView {};
    auto longText = TextView {};

    if (!shortText.build() || !longText.build())
        return;

    shortText.text = "ii";
    longText.text = "iiiiiiii";

    auto shortImage = shortText.renderToImage(1.f);
    auto longImage = longText.renderToImage(1.f);

    check(shortImage.isValid() && longImage.isValid());
    check(rightmostInk(longImage) > rightmostInk(shortImage));
    check(inkPixels(longImage) > inkPixels(shortImage));
};

// Glyphs sit on a shared baseline rather than at the top of the view, which is
// what the bearing offsets are for. Ink must avoid the very top of the target.
auto tGlyphsSitOnTheBaseline = test("TextRendering/glyphsSitOnABaseline") = []
{
    if (!GPU::Device::shared().isValid())
        return;

    auto view = TextView {};

    if (!view.build())
        return;

    view.text = "xxxx";

    auto image = view.renderToImage(1.f);
    check(image.isValid());

    auto topInk = 0;

    for (auto x = 0; x < image.width(); ++x)
        if (image.at(x, 0).r > 0.35f)
            ++topInk;

    // An 'x' has no ascender, so with the baseline at the face's ascent it
    // cannot reach row 0.
    check(topInk == 0);
    check(inkPixels(image) > 20);
};

// Drawing the same text twice must produce the same picture: the second frame
// hits the cache and uploads nothing, and any staleness in the dirty-region
// bookkeeping would show as a difference.
auto tSecondFrameMatchesFirst = test("TextRendering/cachedSecondFrameIsIdentical") = []
{
    if (!GPU::Device::shared().isValid())
        return;

    auto view = TextView {};

    if (!view.build())
        return;

    view.text = "cache";

    auto first = view.renderToImage(1.f);
    auto second = view.renderToImage(1.f);

    check(first.isValid() && second.isValid());
    check(first.width() == second.width());

    auto differing = 0;

    for (auto y = 0; y < first.height(); ++y)
        for (auto x = 0; x < first.width(); ++x)
            if (std::abs(first.at(x, y).r - second.at(x, y).r) > 0.01f)
                ++differing;

    check(differing == 0);
};

// A glyph rasterized *after* the atlas has already uploaded once must still
// appear — the case that exercises the incremental sub-region upload rather
// than the initial full one.
auto tLaterGlyphsStillUpload = test("TextRendering/glyphsAddedAfterFirstUploadStillDraw") = []
{
    if (!GPU::Device::shared().isValid())
        return;

    auto view = TextView {};

    if (!view.build())
        return;

    view.text = "aaa";
    auto first = view.renderToImage(1.f);
    check(first.isValid());

    // Entirely new glyphs, so the atlas grows and re-uploads incrementally.
    view.text = "WWW";
    auto second = view.renderToImage(1.f);

    check(second.isValid());
    check(inkPixels(second) > 20);

    // 'W' is much wider than 'a', so the picture genuinely changed.
    check(rightmostInk(second) > rightmostInk(first));
};
