#include "Common.h"

#include <algorithm>
#include <cctype>

// Shaping through the real platform: CTLine on Apple, IDWriteTextLayout on
// Windows. Like the rasterizer tests these cannot assert exact numbers - that
// would test the font vendor's kerning table - so they assert what the seam
// promises: a kerned pair is narrower than its glyphs, a ligature is one
// glyph, clusters map back to bytes, fallback reaches another face, and a
// weight the family has draws differently from its neighbours. Self-skips
// when a family is not on the machine.

using namespace nano;
using namespace eacp;
using namespace eacp::Text;

namespace
{
// A proportional face both platforms ship, with kerning pairs.
constexpr const char* proportionalFamily()
{
    if constexpr (Platform::isWindows())
        return "Arial";
    else
        return "Helvetica";
}

// A face that ligates "fi".
constexpr const char* ligatingFamily()
{
    if constexpr (Platform::isWindows())
        return "Calibri";
    else
        return "Helvetica";
}

// A family with a light face as well as a bold one.
constexpr const char* weightedFamily()
{
    if constexpr (Platform::isWindows())
        return "Segoe UI";
    else
        return "Helvetica Neue";
}

FontRequest requestFor(const char* family, float pointSize = 32.f)
{
    auto request = FontRequest {};
    request.family = family;
    request.pointSize = pointSize;
    request.scale = 1.f;

    return request;
}

bool sameIgnoringCase(std::string a, std::string b)
{
    for (auto& c: a)
        c = (char) std::tolower((unsigned char) c);

    for (auto& c: b)
        c = (char) std::tolower((unsigned char) c);

    return a == b;
}

// The platform has the family itself, not a substitute for it.
bool has(const GlyphRasterizer& rasterizer, const char* family)
{
    return rasterizer.isValid()
           && sameIgnoringCase(rasterizer.resolvedFamily(), family);
}

long long ink(const GlyphBitmap& bitmap)
{
    long long total = 0;

    for (const auto value: bitmap.pixels)
        total += value;

    return total;
}
} // namespace

auto tKernedPairIsNarrower = test("Shaping/kernedPairIsNarrowerThanItsGlyphs") = []
{
    const auto rasterizer = GlyphRasterizer {requestFor(proportionalFamily())};

    if (!has(rasterizer, proportionalFamily()))
        return;

    const auto pair = rasterizer.shape("AV", {});
    const auto a = rasterizer.shape("A", {});
    const auto v = rasterizer.shape("V", {});

    check(pair.glyphs.size() == 2);
    check(pair.advance > 0.f);
    check(pair.advance < a.advance + v.advance - 0.1f);

    // The second glyph sits where the kerning put it, not at A's advance.
    check(pair.glyphs[1].x < a.advance - 0.1f);
};

auto tLigatureIsOneGlyph = test("Shaping/ligatureIsOneGlyph") = []
{
    const auto rasterizer = GlyphRasterizer {requestFor(ligatingFamily())};

    if (!has(rasterizer, ligatingFamily()))
        return;

    const auto fi = rasterizer.shape("fi", {});

    check(fi.glyphs.size() == 1);
    check(fi.glyphs[0].cluster == 0);
    check(fi.advance > 0.f);

    const auto bitmap = rasterizer.rasterize(fi.glyphs[0].key, {}, {});
    check(bitmap.valid && !bitmap.isEmpty());
};

auto tClustersMapToBytes = test("Shaping/clustersAreByteOffsets") = []
{
    const auto rasterizer = GlyphRasterizer {requestFor(proportionalFamily())};

    if (!has(rasterizer, proportionalFamily()))
        return;

    const auto ascii = rasterizer.shape("ab", {});
    check(ascii.glyphs.size() == 2);
    check(ascii.glyphs[0].cluster == 0);
    check(ascii.glyphs[1].cluster == 1);

    // e-acute is two bytes; whatever it shapes to starts at byte 1.
    const auto accented = rasterizer.shape("a\xc3\xa9", {});
    check(accented.glyphs.size() >= 2);
    check(accented.glyphs[0].cluster == 0);
    check(accented.glyphs[1].cluster == 1);
    check(accented.advance > ascii.glyphs[1].x);
};

// Han, Hiragana and Cyrillic are not in Helvetica or Arial; the shaper
// reaches another face for them and names it, and the glyph rasterizes from
// that face.
auto tFallbackRunsAreNumbered = test("Shaping/fallbackFacesAreNumbered") = []
{
    const auto rasterizer = GlyphRasterizer {requestFor(proportionalFamily())};

    if (!has(rasterizer, proportionalFamily()))
        return;

    const auto run = rasterizer.shape("a\xe6\xbc\xa2z", {});

    check(run.glyphs.size() == 3);
    check(run.glyphs[0].key.font == 0);
    check(run.glyphs[1].key.font != 0);
    check(run.glyphs[2].key.font == 0);
    check(run.glyphs[1].cluster == 1);
    check(run.glyphs[2].cluster == 4);

    const auto han = rasterizer.rasterize(run.glyphs[1].key, {}, {});
    check(han.valid);
    check(!han.isEmpty());
    check(han.advance > 0.f);

    // Met again, the same face gets the same number.
    const auto again = rasterizer.shape("\xe6\xbc\xa2", {});
    check(again.glyphs.size() == 1);
    check(again.glyphs[0].key.font == run.glyphs[1].key.font);
};

auto tWeightsDrawDifferently =
    test("Shaping/weightsTheFamilyHasDrawDifferently") = []
{
    const auto rasterizer = GlyphRasterizer {requestFor(weightedFamily())};

    if (!has(rasterizer, weightedFamily()))
        return;

    const auto at = [&](int weight)
    {
        const auto run = rasterizer.shape("H", {weight, false});

        return run.glyphs.empty()
                   ? GlyphBitmap {}
                   : rasterizer.rasterize(run.glyphs[0].key, {weight, false}, {});
    };

    const auto light = at(300);
    const auto regular = at(400);
    const auto bold = at(700);

    check(light.valid && regular.valid && bold.valid);
    check(ink(light) < ink(regular));
    check(ink(regular) < ink(bold));

    // 650 is nearest 700 by CSS's rules, so it is the same face.
    check(ink(at(650)) == ink(bold));
};

// The codepoint convenience is the shaped path for one codepoint, so a walk
// by codepoint and a shaped run agree about a lone glyph.
auto tCodepointRasterizesThroughShaping =
    test("Shaping/oneCodepointRasterizesAsItsShapedGlyph") = []
{
    const auto rasterizer = GlyphRasterizer {requestFor(proportionalFamily())};

    if (!has(rasterizer, proportionalFamily()))
        return;

    const auto walked = rasterizer.rasterize(U'A', FontStyle::Regular);
    const auto shaped = rasterizer.shape("A", {});

    check(walked.valid && !walked.isEmpty());
    check(shaped.glyphs.size() == 1);
    check(std::abs(walked.advance - shaped.advance) < 0.01f);
};

// The renderer measures what the shaper places, so a kerned pair is narrower
// than the sum of its parts there too - which is what layout sees.
auto tRendererMeasuresKerning = test("Shaping/rendererMeasuresTheKernedPair") = []
{
    if (!has(GlyphRasterizer {requestFor(proportionalFamily())},
             proportionalFamily()))
        return;

    auto renderer = TextRenderer {32.f, proportionalFamily()};
    renderer.setViewport({320.f, 64.f}, 1.f);

    const auto font = Font {proportionalFamily(), 32.f};
    const auto pair = renderer.measure("AV", font);
    const auto parts = renderer.measure("A", font) + renderer.measure("V", font);

    check(pair > 0.f);
    check(pair < parts - 0.1f);

    // A weight asked for by number is a face of its own.
    auto light = font;
    light.weight = 300;
    check(renderer.measure("AV", light) > 0.f);
};

// A family's heaviest faces are often its condensed ones (Helvetica Neue
// ships Condensed Black and nothing wider above Bold). CSS matches by
// stretch before weight, so a 900 in such a family is its bold, not a
// narrow face: the glyph is as wide as the bold's, not narrower.
auto tWidthBeforeWeight =
    test("Shaping/normalWidthIsPreferredToAHeavierCondensedFace") = []
{
    const auto rasterizer = GlyphRasterizer {requestFor(weightedFamily())};

    if (!has(rasterizer, weightedFamily()))
        return;

    const auto bold = rasterizer.shape("M", {700, false});
    const auto black = rasterizer.shape("M", {900, false});

    check(bold.glyphs.size() == 1 && black.glyphs.size() == 1);
    check(black.advance >= bold.advance - 0.01f);
};
