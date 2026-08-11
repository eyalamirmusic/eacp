#include "Common.h"

#include <eacp/Sprites/Sprites.h>

#include <optional>
#include <string>

using namespace nano;
using namespace eacp;
using namespace eacp::Text;

namespace
{
constexpr auto viewWidth = 320.f;
constexpr auto viewHeight = 64.f;

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

        if (!GlyphRasterizer {request}.isValid())
            return false;

        atlas = makeOwned<GlyphAtlas>(rasterizerFaceFactory(), request, 256, 1024);

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
            const auto glyph =
                atlas->glyph((char32_t) character, FontStyle::Regular);

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

int rightmostInk(const Graphics::Image& image)
{
    for (auto x = image.width() - 1; x >= 0; --x)
        for (auto y = 0; y < image.height(); ++y)
            if (image.at(x, y).r > 0.35f)
                return x;

    return -1;
}
} // namespace

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

auto tEmptyTextDrawsNothing =
    test("TextRendering/emptyTextLeavesTheTargetClear") = []
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

auto tSpacesAdvanceWithoutInk =
    test("TextRendering/spacesAdvanceThePenWithoutDrawing") = []
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

    check(rightmostInk(spaced) > rightmostInk(tight));

    const auto tightInk = inkPixels(tight);
    const auto spacedInk = inkPixels(spaced);

    check(spacedInk > tightInk / 2);
    check(spacedInk < tightInk * 2);
};

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

auto tSecondFrameMatchesFirst =
    test("TextRendering/cachedSecondFrameIsIdentical") = []
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

auto tLaterGlyphsStillUpload =
    test("TextRendering/glyphsAddedAfterFirstUploadStillDraw") = []
{
    if (!GPU::Device::shared().isValid())
        return;

    auto view = TextView {};

    if (!view.build())
        return;

    view.text = "aaa";
    auto first = view.renderToImage(1.f);
    check(first.isValid());

    view.text = "WWW";
    auto second = view.renderToImage(1.f);

    check(second.isValid());
    check(inkPixels(second) > 20);

    check(rightmostInk(second) > rightmostInk(first));
};
