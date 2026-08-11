#include "Common.h"

#include <algorithm>

using namespace nano;
using namespace eacp;
using namespace eacp::Text;

namespace
{
// Naming one platform's family here would make the other skip every test below.
FontRequest monospaceRequest(float pointSize = 16.f, float scale = 1.f)
{
    auto request = FontRequest {};
    request.family = defaultMonospaceFamily();
    request.pointSize = pointSize;
    request.scale = scale;

    return request;
}

int maxCoverage(const GlyphBitmap& bitmap)
{
    if (bitmap.format == GlyphFormat::Mask)
        return bitmap.pixels.empty()
                   ? 0
                   : *std::max_element(bitmap.pixels.begin(), bitmap.pixels.end());

    auto highest = 0;

    for (std::size_t i = 3; i < bitmap.pixels.size(); i += 4)
        highest = std::max(highest, (int) bitmap.pixels[i]);

    return highest;
}
} // namespace

auto tResolvesASystemFont = test("GlyphRasterizer/resolvesASystemFont") = []
{
    const auto rasterizer = GlyphRasterizer {monospaceRequest()};

    if (!rasterizer.isValid())
        return;

    const auto metrics = rasterizer.metrics(FontStyle::Regular);

    check(metrics.ascent > 0.f);
    check(metrics.descent > 0.f);
    check(metrics.advance > 0.f);
    check(metrics.lineHeight() > metrics.ascent);
};

auto tMetricsScaleWithPixelSize =
    test("GlyphRasterizer/metricsScaleWithPixelSize") = []
{
    const auto small = GlyphRasterizer {monospaceRequest(16.f, 1.f)};
    const auto large = GlyphRasterizer {monospaceRequest(16.f, 2.f)};

    if (!small.isValid() || !large.isValid())
        return;

    const auto oneX = small.metrics(FontStyle::Regular);
    const auto twoX = large.metrics(FontStyle::Regular);

    check(twoX.ascent > oneX.ascent * 1.8f);
    check(twoX.ascent < oneX.ascent * 2.2f);
    check(small.scale() == 1.f);
    check(large.scale() == 2.f);
};

auto tRasterizesALetter = test("GlyphRasterizer/rasterizesALetterAsAMask") = []
{
    const auto rasterizer = GlyphRasterizer {monospaceRequest()};

    if (!rasterizer.isValid())
        return;

    const auto bitmap = rasterizer.rasterize(U'A', FontStyle::Regular);

    check(bitmap.valid);
    check(!bitmap.isEmpty());
    check(bitmap.format == GlyphFormat::Mask);
    check(bitmap.advance > 0.f);

    check(bitmap.pixels.size() == (std::size_t) bitmap.width * bitmap.height);

    check(maxCoverage(bitmap) > 0);
    check(bitmap.width < 200);
    check(bitmap.height < 200);
};

auto tSpaceIsValidButEmpty = test("GlyphRasterizer/spaceIsValidButDrawsNothing") = []
{
    const auto rasterizer = GlyphRasterizer {monospaceRequest()};

    if (!rasterizer.isValid())
        return;

    const auto bitmap = rasterizer.rasterize(U' ', FontStyle::Regular);

    check(bitmap.valid);
    check(bitmap.isEmpty());
    check(bitmap.advance > 0.f);
};

auto tMonospaceAdvancesMatch =
    test("GlyphRasterizer/monospaceGlyphsShareAnAdvance") = []
{
    const auto rasterizer = GlyphRasterizer {monospaceRequest()};

    if (!rasterizer.isValid())
        return;

    const auto reference = rasterizer.rasterize(U'M', FontStyle::Regular).advance;

    for (const auto codepoint: {U'i', U'W', U'.', U'0'})
    {
        const auto advance =
            rasterizer.rasterize(codepoint, FontStyle::Regular).advance;
        check(std::abs(advance - reference) < 0.5f);
    }
};

auto tBearingsDescribeTheBaseline =
    test("GlyphRasterizer/bearingsPlaceGlyphsOnTheBaseline") = []
{
    const auto rasterizer = GlyphRasterizer {monospaceRequest(32.f)};

    if (!rasterizer.isValid())
        return;

    const auto x = rasterizer.rasterize(U'x', FontStyle::Regular);
    const auto p = rasterizer.rasterize(U'p', FontStyle::Regular);

    check(x.valid && p.valid);

    // bearingY is the top edge above the baseline; height reaches down from it.
    check(x.bearingY - (float) x.height <= 0.5f);
    check(p.bearingY - (float) p.height < -0.5f);
};

auto tStylesProduceDifferentGlyphs =
    test("GlyphRasterizer/boldDiffersFromRegular") = []
{
    const auto rasterizer = GlyphRasterizer {monospaceRequest(32.f)};

    if (!rasterizer.isValid())
        return;

    const auto regular = rasterizer.rasterize(U'H', FontStyle::Regular);
    const auto bold = rasterizer.rasterize(U'H', FontStyle::Bold);

    check(regular.valid && bold.valid);

    const auto ink = [](const GlyphBitmap& bitmap)
    {
        long long total = 0;

        for (const auto value: bitmap.pixels)
            total += value;

        return total;
    };

    check(ink(bold) > ink(regular));
};

// Not asserted: that an unassigned codepoint reports invalid. Apple's fallback
// reaches the Last Resort face, which draws a box for anything.
auto tUnassignedCodepointIsSelfConsistent =
    test("GlyphRasterizer/unassignedCodepointIsSelfConsistent") = []
{
    const auto rasterizer = GlyphRasterizer {monospaceRequest()};

    if (!rasterizer.isValid())
        return;

    for (const auto codepoint:
         {(char32_t) 0x10FFFD, (char32_t) 0xE000, (char32_t) 0xFFFF})
    {
        const auto bitmap = rasterizer.rasterize(codepoint, FontStyle::Regular);

        if (!bitmap.valid)
            continue;

        check(bitmap.pixels.size()
              == (std::size_t) bitmap.width * bitmap.height
                     * bytesPerPixel(bitmap.format));
    }
};

auto tFallsBackForMissingGlyphs =
    test("GlyphRasterizer/fallsBackToAnotherFaceForMissingGlyphs") = []
{
    const auto rasterizer = GlyphRasterizer {monospaceRequest(24.f)};

    if (!rasterizer.isValid())
        return;

    // Escapes because the build does not force a UTF-8 source encoding.
    for (const auto codepoint: {U'\u6f22', U'\u3042', U'\u0416'})
    {
        const auto bitmap = rasterizer.rasterize(codepoint, FontStyle::Regular);

        check(bitmap.valid);
        check(!bitmap.isEmpty());
        check(bitmap.advance > 0.f);
        check(bitmap.pixels.size()
              == (std::size_t) bitmap.width * bitmap.height
                     * bytesPerPixel(bitmap.format));
    }
};

// Not every system resolves emoji to a colour face, so this asserts the format
// contract rather than demanding colour.
auto tColorGlyphsReportFourBytesPerPixel =
    test("GlyphRasterizer/colorGlyphsAreSelfConsistent") = []
{
    const auto rasterizer = GlyphRasterizer {monospaceRequest(24.f)};

    if (!rasterizer.isValid())
        return;

    const auto bitmap = rasterizer.rasterize(U'\U0001F600', FontStyle::Regular);

    if (!bitmap.valid || bitmap.isEmpty())
        return;

    check(bitmap.pixels.size()
          == (std::size_t) bitmap.width * bitmap.height
                 * bytesPerPixel(bitmap.format));

    if (bitmap.format != GlyphFormat::Color)
        return;

    auto opaque = 0;

    for (std::size_t i = 3; i < bitmap.pixels.size(); i += 4)
        if (bitmap.pixels[i] > 128)
            ++opaque;

    check(opaque > 0);
};

auto tAtlasAcceptsUnassignedCodepoints =
    test("GlyphRasterizer/atlasAcceptsUnassignedCodepoints") = []
{
    if (!GlyphRasterizer {monospaceRequest()}.isValid())
        return;

    auto atlas = GlyphAtlas {rasterizerFaceFactory(), monospaceRequest(), 256, 1024};

    atlas.glyph((char32_t) 0x10FFFD, FontStyle::Regular);
    atlas.glyph((char32_t) 0xE000, FontStyle::Regular);

    const auto letter = atlas.glyph(U'A', FontStyle::Regular);

    check(letter.valid);
    check(letter.src.w > 0.f);
};

auto tAtlasWorksOverRealRasterizer =
    test("GlyphRasterizer/atlasCachesRealGlyphs") = []
{
    if (!GlyphRasterizer {monospaceRequest()}.isValid())
        return;

    auto atlas = GlyphAtlas {rasterizerFaceFactory(), monospaceRequest(), 256, 1024};

    const auto first = atlas.glyph(U'A', FontStyle::Regular);

    check(first.valid);
    check(first.src.w > 0.f);
    check(first.advance > 0.f);

    const auto again = atlas.glyph(U'A', FontStyle::Regular);

    check(again.src.x == first.src.x);
    check(again.src.y == first.src.y);

    for (char32_t codepoint = U'!'; codepoint <= U'~'; ++codepoint)
        atlas.glyph(codepoint, FontStyle::Regular);

    check(atlas.generation() == 0);
};

auto tRealFacesDifferBySize =
    test("GlyphRasterizer/atlasHoldsTwoSizesOfOneFamily") = []
{
    if (!GlyphRasterizer {monospaceRequest()}.isValid())
        return;

    auto atlas = GlyphAtlas {rasterizerFaceFactory(), monospaceRequest(), 256, 1024};

    const auto small = atlas.findOrAddFace(defaultMonospaceFamily(), 12.f);
    const auto large = atlas.findOrAddFace(defaultMonospaceFamily(), 24.f);

    check(small != large);

    const auto a = atlas.glyph(U'A', FontStyle::Regular, small);
    const auto b = atlas.glyph(U'A', FontStyle::Regular, large);

    check(a.valid);
    check(b.valid);

    check(b.advance > a.advance * 1.5f);
    check(b.src.h > a.src.h);

    check(a.src.x != b.src.x || a.src.y != b.src.y);

    check(atlas.metrics(FontStyle::Regular, large).lineHeight()
          > atlas.metrics(FontStyle::Regular, small).lineHeight());
};

auto tRealAtlasTakesASecondFamily =
    test("GlyphRasterizer/atlasHoldsTwoFamilies") = []
{
    if (!GlyphRasterizer {monospaceRequest()}.isValid())
        return;

    auto atlas = GlyphAtlas {rasterizerFaceFactory(), monospaceRequest(), 256, 1024};

    auto proportional = FontRequest {};
    proportional.family = "Helvetica";
    proportional.pointSize = 16.f;

    if (!GlyphRasterizer {proportional}.isValid())
        return;

    const auto other =
        atlas.findOrAddFace(proportional.family, proportional.pointSize);

    check(other != 0);
    check(atlas.glyph(U'W', FontStyle::Regular, other).valid);

    const auto narrow = atlas.glyph(U'i', FontStyle::Regular, other);
    const auto wide = atlas.glyph(U'W', FontStyle::Regular, other);

    check(narrow.advance < wide.advance);
    check(atlas.glyph(U'i', FontStyle::Regular).advance
          == atlas.glyph(U'W', FontStyle::Regular).advance);
};

auto tRendererMeasuresPerFont =
    test("GlyphRasterizer/rendererMeasuresTheFaceItIsGiven") = []
{
    if (!GlyphRasterizer {monospaceRequest()}.isValid())
        return;

    auto renderer = TextRenderer {13.f, defaultMonospaceFamily()};
    renderer.setViewport({320.f, 64.f}, 1.f);

    const auto text = std::string_view {"measure me"};

    const auto small = renderer.measure(text, Font {defaultMonospaceFamily(), 12.f});
    const auto large = renderer.measure(text, Font {defaultMonospaceFamily(), 24.f});

    check(small > 0.f);
    check(large > small * 1.5f);

    renderer.setFont({defaultMonospaceFamily(), 24.f});

    check(renderer.measure(text) == large);
    check(renderer.ascent()
          == renderer.ascent(Font {defaultMonospaceFamily(), 24.f}));

    const auto bold = renderer.measure(
        text, Font {defaultMonospaceFamily(), 24.f, FontStyle::Bold});

    check(bold > 0.f);
};
