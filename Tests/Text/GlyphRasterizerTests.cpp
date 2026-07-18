#include "Common.h"

#include <algorithm>

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
// Present on macOS, iOS and (as a metric-compatible substitute) most systems
// eacp targets. A monospace face keeps the advance assertions meaningful.
FontRequest monospaceRequest(float pointSize = 16.f, float scale = 1.f)
{
    auto request = FontRequest {};
    request.family = "Menlo";
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
auto tMetricsScaleWithPixelSize = test("GlyphRasterizer/metricsScaleWithPixelSize") = []
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

    check(bitmap.pixels.size()
          == (std::size_t) bitmap.width * bitmap.height);

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
auto tMonospaceAdvancesMatch = test("GlyphRasterizer/monospaceGlyphsShareAnAdvance") = []
{
    const auto rasterizer = GlyphRasterizer {monospaceRequest()};

    if (!rasterizer.isValid())
        return;

    const auto reference = rasterizer.rasterize(U'M', FontStyle::Regular).advance;

    for (const auto codepoint: {U'i', U'W', U'.', U'0'})
    {
        const auto advance = rasterizer.rasterize(codepoint, FontStyle::Regular).advance;
        check(std::abs(advance - reference) < 0.5f);
    }
};

// Bearings are what CowTerm's atlas lacked. A letter with a descender must
// extend below the baseline, and one without must not — the sign convention
// being wrong would push half the text off its line.
auto tBearingsDescribeTheBaseline = test("GlyphRasterizer/bearingsPlaceGlyphsOnTheBaseline") = []
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

auto tStylesProduceDifferentGlyphs = test("GlyphRasterizer/boldDiffersFromRegular") = []
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

    for (const auto codepoint: {(char32_t) 0x10FFFD, (char32_t) 0xE000, (char32_t) 0xFFFF})
    {
        const auto bitmap = rasterizer.rasterize(codepoint, FontStyle::Regular);

        if (!bitmap.valid)
            continue;

        check(bitmap.pixels.size()
              == (std::size_t) bitmap.width * bitmap.height
                     * bytesPerPixel(bitmap.format));
    }
};

// The same codepoints must survive the atlas without corrupting it, which is
// the path that would actually break if a bitmap misreported its size.
auto tAtlasAcceptsUnassignedCodepoints =
    test("GlyphRasterizer/atlasAcceptsUnassignedCodepoints") = []
{
    auto rasterizer = makeOwned<GlyphRasterizer>(monospaceRequest());

    if (!rasterizer->isValid())
        return;

    auto atlas = GlyphAtlas {OwningPointer<GlyphSource> {std::move(rasterizer)}, 256, 1024};

    atlas.glyph((char32_t) 0x10FFFD, FontStyle::Regular);
    atlas.glyph((char32_t) 0xE000, FontStyle::Regular);

    // A known-good glyph still works afterwards.
    const auto letter = atlas.glyph(U'A', FontStyle::Regular);

    check(letter.valid);
    check(letter.src.w > 0.f);
};

// The atlas on top of the real rasterizer: the end-to-end path, minus the GPU.
auto tAtlasWorksOverRealRasterizer = test("GlyphRasterizer/atlasCachesRealGlyphs") = []
{
    auto rasterizer = makeOwned<GlyphRasterizer>(monospaceRequest());

    if (!rasterizer->isValid())
        return;

    auto atlas = GlyphAtlas {OwningPointer<GlyphSource> {std::move(rasterizer)}, 256, 1024};

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
