#include "Common.h"

#include <eacp/Sprites/Sprites.h>

#include <set>

// GlyphRenderer: the piece that makes a GlyphAtlas drawable.
//
// The case that matters most is the one that motivated it. The mask atlas is
// R8Unorm, which samples as (coverage, 0, 0, 1) — coverage in red, alpha
// pinned to 1. Drawn through a general sprite shader, which multiplies the
// sample by a tint, text comes out opaque red instead of tinted. So these tests
// assert on the *colour* that reaches the target, not merely that something was
// drawn.
//
// Self-skips without a GPU device or a resolvable font.

using namespace nano;
using namespace eacp;
using namespace eacp::Text;

namespace
{
constexpr auto viewSize = 128.f;

struct GlyphView final : GPU::GPUView
{
    GlyphView()
    {
        setSampleCount(1);
        setBounds({0.f, 0.f, viewSize, viewSize});
    }

    bool build()
    {
        auto request = FontRequest {};
        request.family = defaultMonospaceFamily();
        request.pointSize = 48.f;
        request.scale = 1.f;

        auto rasterizer = makeOwned<GlyphRasterizer>(request);

        if (!rasterizer->isValid())
            return false;

        atlas = makeOwned<GlyphAtlas>(
            OwningPointer<GlyphSource> {std::move(rasterizer)}, 256, 1024);

        renderer.emplace();
        renderer->setViewportSize({viewSize, viewSize});

        return true;
    }

    void render(GPU::Frame& frame) override
    {
        auto pass = frame.beginPass({{0.f, 0.f, 0.f, 1.f}});

        if (!atlas || !renderer)
            return;

        const auto metrics = atlas->metrics();
        const auto glyph = atlas->glyph(codepoint, FontStyle::Regular);

        atlas->commit();

        if (!glyph.valid || glyph.empty)
            return;

        renderer->begin();
        renderer->add({8.f + glyph.offset.x,
                       metrics.ascent + glyph.offset.y,
                       glyph.src.w,
                       glyph.src.h},
                      glyph.src,
                      color,
                      glyph.format == GlyphFormat::Color);

        renderer->flush(pass, *atlas);
    }

    char32_t codepoint = U'M';
    Graphics::Color color = Graphics::Color::white();

    OwningPointer<GlyphAtlas> atlas;
    std::optional<GlyphRenderer> renderer;
};

// The brightest pixel, which for a glyph drawn on black is its own colour at
// full coverage.
Graphics::Color brightest(const Graphics::Image& image)
{
    auto best = Graphics::Color {0.f, 0.f, 0.f, 1.f};
    auto bestSum = 0.f;

    for (auto y = 0; y < image.height(); ++y)
    {
        for (auto x = 0; x < image.width(); ++x)
        {
            const auto pixel = image.at(x, y);
            const auto sum = pixel.r + pixel.g + pixel.b;

            if (sum > bestSum)
            {
                bestSum = sum;
                best = pixel;
            }
        }
    }

    return best;
}

int inkPixels(const Graphics::Image& image)
{
    auto total = 0;

    for (auto y = 0; y < image.height(); ++y)
        for (auto x = 0; x < image.width(); ++x)
            if (image.at(x, y).r + image.at(x, y).g + image.at(x, y).b > 0.3f)
                ++total;

    return total;
}
} // namespace

auto tDrawsAGlyph = test("GlyphRenderer/drawsAGlyphFromTheAtlas") = []
{
    if (!GPU::Device::shared().isValid())
        return;

    auto view = GlyphView {};

    if (!view.build())
        return;

    auto image = view.renderToImage(1.f);

    check(image.isValid());
    check(inkPixels(image) > 20);
};

// The regression this class exists to prevent. A mask drawn through a general
// sprite shader arrives opaque red; drawn correctly it takes the colour asked
// for. Anything that reintroduces `sample * tint` fails here.
auto tMaskTakesTheRequestedColour = test("GlyphRenderer/maskGlyphsTakeTheRequestedColour") = []
{
    if (!GPU::Device::shared().isValid())
        return;

    auto view = GlyphView {};

    if (!view.build())
        return;

    // A colour no channel of a red-bug artefact could produce.
    view.color = {0.25f, 0.85f, 0.35f};

    auto image = view.renderToImage(1.f);
    check(image.isValid());

    const auto ink = brightest(image);

    check(ink.g > 0.5f);       // the green it was asked for
    check(ink.r < ink.g);      // and not the red an R8 sample would give
    check(ink.b < ink.g);
};

// Colour is per glyph, so a run of text can change colour mid-line without a
// separate draw call.
auto tColourIsPerGlyph = test("GlyphRenderer/differentColoursProduceDifferentPixels") = []
{
    if (!GPU::Device::shared().isValid())
        return;

    auto blue = GlyphView {};
    auto red = GlyphView {};

    if (!blue.build() || !red.build())
        return;

    blue.color = {0.2f, 0.3f, 0.9f};
    red.color = {0.9f, 0.3f, 0.2f};

    auto blueImage = blue.renderToImage(1.f);
    auto redImage = red.renderToImage(1.f);

    check(blueImage.isValid() && redImage.isValid());

    check(brightest(blueImage).b > brightest(blueImage).r);
    check(brightest(redImage).r > brightest(redImage).b);
};

// Coverage has to blend, or antialiased glyph edges punch holes in the
// background instead of easing into it.
auto tEdgesBlend = test("GlyphRenderer/antialiasedEdgesBlend") = []
{
    if (!GPU::Device::shared().isValid())
        return;

    auto view = GlyphView {};

    if (!view.build())
        return;

    view.color = Graphics::Color::white();

    auto image = view.renderToImage(1.f);
    check(image.isValid());

    // Partly covered pixels: neither background nor full coverage. Their
    // existence is what proves the alpha ramp survived to the target.
    auto partial = 0;

    for (auto y = 0; y < image.height(); ++y)
        for (auto x = 0; x < image.width(); ++x)
        {
            const auto value = image.at(x, y).r;

            if (value > 0.15f && value < 0.85f)
                ++partial;
        }

    check(partial > 5);
};

auto tEmptyBatchDrawsNothing = test("GlyphRenderer/nothingQueuedDrawsNothing") = []
{
    if (!GPU::Device::shared().isValid())
        return;

    auto view = GlyphView {};

    if (!view.build())
        return;

    view.codepoint = U' '; // valid but empty, so nothing is queued

    auto image = view.renderToImage(1.f);

    check(image.isValid());
    check(inkPixels(image) == 0);
};

auto tQueueTracksAdds = test("GlyphRenderer/queueCountsWhatWasAdded") = []
{
    auto renderer = GlyphRenderer {};

    renderer.begin();
    check(renderer.queuedGlyphs() == 0);

    renderer.add({0.f, 0.f, 10.f, 10.f}, {0.f, 0.f, 8.f, 8.f}, Graphics::Color::white(), false);
    renderer.add({10.f, 0.f, 10.f, 10.f}, {0.f, 0.f, 8.f, 8.f}, Graphics::Color::white(), true);

    check(renderer.queuedGlyphs() == 2);

    renderer.begin();
    check(renderer.queuedGlyphs() == 0);
};

// A degenerate destination is dropped rather than queued as a zero-area quad.
auto tSkipsEmptyRects = test("GlyphRenderer/emptyDestinationsAreSkipped") = []
{
    auto renderer = GlyphRenderer {};

    renderer.begin();
    renderer.add({0.f, 0.f, 0.f, 10.f}, {0.f, 0.f, 8.f, 8.f}, Graphics::Color::white(), false);
    renderer.add({0.f, 0.f, 10.f, 0.f}, {0.f, 0.f, 8.f, 8.f}, Graphics::Color::white(), false);

    check(renderer.queuedGlyphs() == 0);
};
