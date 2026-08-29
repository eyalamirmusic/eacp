#include "Common.h"

#include <algorithm>
#include <cctype>
#include <cmath>

// The real platform rasterizer, against a font the OS is guaranteed to have.
//
// These cannot assert exact pixel values — that would be a test of the font
// vendor's outlines and of this year's CoreText — so they assert the contract
// the atlas actually depends on: that coverage lands somewhere in the bitmap,
// that the geometry is self-consistent, and that the format is right for the
// kind of glyph. Self-skips if the family cannot be resolved.

using namespace nano;
using namespace eacp;
using namespace eacp::Text;

namespace
{
// Whichever fixed-pitch face the platform ships — Menlo on Apple, Consolas on
// Windows. A monospace face keeps the advance assertions meaningful, and naming
// one platform's family here would make the other skip every test below.
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

// Metrics must track the pixel size, since that is how the atlas stays crisp on
// a Retina panel: same points, twice the pixels.
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

// A letter must actually produce ink, in a mask format, with a bitmap no larger
// than a sane multiple of the em — the check that would catch a rasterizer
// writing nothing, or writing into a wrongly sized buffer.
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

    check(maxCoverage(bitmap) > 0); // it drew something
    check(bitmap.width < 200);
    check(bitmap.height < 200);
};

// A space is the case that separates "valid but blank" from "no such glyph".
// Getting this wrong means either re-rasterizing every space forever or losing
// the advance and collapsing all whitespace.
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

// On a monospace face every glyph steps the pen by the same amount; that is the
// property a terminal grid is built on.
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

// Bearings are what CowTerm's atlas lacked. A letter with a descender must
// extend below the baseline, and one without must not — the sign convention
// being wrong would push half the text off its line.
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
    // 'x' sits on the line, 'p' hangs below it.
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

    // A bold face puts down more ink. Comparing coverage totals avoids
    // depending on the exact outlines.
    const auto ink = [](const GlyphBitmap& bitmap)
    {
        long long total = 0;

        for (const auto value: bitmap.pixels)
            total += value;

        return total;
    };

    check(ink(bold) > ink(regular));
};

// An unassigned codepoint must come back describing itself honestly rather than
// crashing or lying about its size.
//
// Note what this does *not* assert. The obvious expectation — that an
// unassigned codepoint reports invalid — is wrong on Apple: font fallback
// reaches the Last Resort face, which draws a box for anything, so the
// rasterizer legitimately returns a valid, non-empty glyph. That is also the
// better behaviour, since the user sees a visible box instead of a silent gap.
// What must hold either way is that the buffer matches the declared dimensions,
// because the atlas memcpys straight out of it.
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

// A Latin monospace family has no CJK glyphs, so these can only come from the
// system falling back to another face. Without that, half the world's text
// silently disappears rather than rendering from a substitute.
auto tFallsBackForMissingGlyphs =
    test("GlyphRasterizer/fallsBackToAnotherFaceForMissingGlyphs") = []
{
    const auto rasterizer = GlyphRasterizer {monospaceRequest(24.f)};

    if (!rasterizer.isValid())
        return;

    // Han, Hiragana, Cyrillic — none of them in Menlo or Consolas. Spelled as
    // escapes because the build does not force a UTF-8 source encoding.
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

// Emoji come from a colour font, which the atlas keeps in a separate RGBA page.
// The format has to be reported honestly or the atlas blits 4-byte pixels into
// a 1-byte page. Not every system resolves emoji to a colour face, so this
// asserts the contract rather than demanding colour.
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

    // A colour glyph that came out fully transparent would draw nothing, which
    // is the failure a premultiply/compositing mistake produces.
    auto opaque = 0;

    for (std::size_t i = 3; i < bitmap.pixels.size(); i += 4)
        if (bitmap.pixels[i] > 128)
            ++opaque;

    check(opaque > 0);
};

// The same codepoints must survive the atlas without corrupting it, which is
// the path that would actually break if a bitmap misreported its size.
auto tAtlasAcceptsUnassignedCodepoints =
    test("GlyphRasterizer/atlasAcceptsUnassignedCodepoints") = []
{
    if (!GlyphRasterizer {monospaceRequest()}.isValid())
        return;

    auto atlas = GlyphAtlas {rasterizerFaceFactory(), monospaceRequest(), 256, 1024};

    atlas.glyph((char32_t) 0x10FFFD, FontStyle::Regular);
    atlas.glyph((char32_t) 0xE000, FontStyle::Regular);

    // A known-good glyph still works afterwards.
    const auto letter = atlas.glyph(U'A', FontStyle::Regular);

    check(letter.valid);
    check(letter.src.w > 0.f);
};

// The atlas on top of the real rasterizer: the end-to-end path, minus the GPU.
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

    // A full printable ASCII run must fit without ever clearing.
    for (char32_t codepoint = U'!'; codepoint <= U'~'; ++codepoint)
        atlas.glyph(codepoint, FontStyle::Regular);

    check(atlas.generation() == 0);
};

// Two sizes of a real face in one atlas, which is what the face table exists
// for: the same character twice, in one texture, at the size each asked for.
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

    // Twice the points, and a monospace advance is proportional to them.
    check(b.advance > a.advance * 1.5f);
    check(b.src.h > a.src.h);

    // Packed side by side rather than on top of each other.
    check(a.src.x != b.src.x || a.src.y != b.src.y);

    check(atlas.metrics(FontStyle::Regular, large).lineHeight()
          > atlas.metrics(FontStyle::Regular, small).lineHeight());
};

// A family the atlas was not built with, asked for after the fact. This is what
// a document mixing a proportional heading with a monospace log needs, and what
// a single-face atlas could not do at all.
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

    // A proportional face gives 'i' and 'W' different advances; the monospace
    // one it shares the atlas with does not.
    const auto narrow = atlas.glyph(U'i', FontStyle::Regular, other);
    const auto wide = atlas.glyph(U'W', FontStyle::Regular, other);

    check(narrow.advance < wide.advance);
    check(atlas.glyph(U'i', FontStyle::Regular).advance
          == atlas.glyph(U'W', FontStyle::Regular).advance);
};

// The renderer's own font-per-call path, which is what UI::Graphics::setFont
// drives: measurement has to follow the face, or a centred caption centres
// against glyphs other than the ones drawn.
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

    // The default face is one of them rather than a third thing.
    renderer.setFont({defaultMonospaceFamily(), 24.f});

    check(renderer.measure(text) == large);
    check(renderer.ascent()
          == renderer.ascent(Font {defaultMonospaceFamily(), 24.f}));

    // And a bold run is measured as bold, not as the regular face beside it.
    const auto bold = renderer.measure(
        text, Font {defaultMonospaceFamily(), 24.f, FontStyle::Bold});

    check(bold > 0.f);
};

// A name the platform has resolves to itself; a name it does not have still
// draws - CoreText and the Windows resolver both substitute - and says which
// family it substituted, which is how a family list is walked to the first
// one that exists.
auto tResolvedFamily =
    test("GlyphRasterizer/resolvedFamilyNamesTheFaceThePlatformChose") = []
{
    auto exact = GlyphRasterizer {monospaceRequest()};

    if (!exact.isValid())
        return;

    auto sameIgnoringCase = [](std::string a, std::string b)
    {
        for (auto& c: a)
            c = (char) std::tolower((unsigned char) c);

        for (auto& c: b)
            c = (char) std::tolower((unsigned char) c);

        return a == b;
    };

    check(sameIgnoringCase(exact.resolvedFamily(), defaultMonospaceFamily()));

    auto request = monospaceRequest();
    request.family = "No Such Family EACP";

    auto substituted = GlyphRasterizer {request};

    check(substituted.isValid(), "a substitute is still a face to draw with");
    check(!substituted.resolvedFamily().empty());
    check(!sameIgnoringCase(substituted.resolvedFamily(), request.family),
          "and it says so");
};

// The subpixel offset moves the ink and nothing else: drawn half a pixel to
// the right, the glyph's centre of mass - the bearing and the bitmap between
// them - is half a pixel further from the pen, with the same ink in it.
auto tSubpixelOffsetMovesTheInk =
    test("GlyphRasterizer/subpixelOffsetMovesTheInkAndKeepsTheMass") = []
{
    const auto rasterizer = GlyphRasterizer {monospaceRequest(24.f)};

    if (!rasterizer.isValid())
        return;

    const auto run = rasterizer.shape("H", {});

    if (run.glyphs.empty())
        return;

    struct Ink
    {
        double mass = 0.0;
        double centre = 0.0;
    };

    auto inkOf = [](const GlyphBitmap& bitmap)
    {
        auto moment = 0.0;
        auto ink = Ink {};

        for (auto y = 0; y < bitmap.height; ++y)
            for (auto x = 0; x < bitmap.width; ++x)
            {
                const auto coverage =
                    (double) bitmap.pixels[(std::size_t) y * bitmap.width + x];

                ink.mass += coverage;
                moment += coverage * ((double) x + 0.5);
            }

        ink.centre = bitmap.bearingX + moment / ink.mass;

        return ink;
    };

    const auto whole = rasterizer.rasterize(run.glyphs[0].key, {}, {});
    const auto half = rasterizer.rasterize(run.glyphs[0].key, {}, {0.5f, false});

    check(whole.valid && !whole.isEmpty());
    check(half.valid && !half.isEmpty());

    const auto before = inkOf(whole);
    const auto after = inkOf(half);

    check(std::abs(after.centre - before.centre - 0.5) < 0.15);
    check(std::abs(after.mass - before.mass) < before.mass * 0.05);
};

// The platform thickens light text more than dark - CoreGraphics by a sixth
// at this size, DirectWrite not at all - so a mask for light text has at
// least the ink of one for dark, and on Apple visibly more.
auto tLightTextIsThickened =
    test("GlyphRasterizer/lightTextHasAtLeastTheInkOfDark") = []
{
    const auto rasterizer = GlyphRasterizer {monospaceRequest(13.f, 2.f)};

    if (!rasterizer.isValid())
        return;

    const auto run = rasterizer.shape("n", {});

    if (run.glyphs.empty())
        return;

    const auto inkOf = [](const GlyphBitmap& bitmap)
    {
        long long total = 0;

        for (const auto value: bitmap.pixels)
            total += value;

        return total;
    };

    const auto dark =
        inkOf(rasterizer.rasterize(run.glyphs[0].key, {}, {0.f, false}));
    const auto light =
        inkOf(rasterizer.rasterize(run.glyphs[0].key, {}, {0.f, true}));

    check(dark > 0);
    check(light >= dark);

#if defined(__APPLE__)
    check(light > dark + dark / 20);
#endif
};
