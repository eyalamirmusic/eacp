#include "Common.h"

// GlyphAtlas driven by a stub GlyphSource.
//
// This is what the rasterizer/atlas split buys: caching, growth, format
// routing, metric conversion and the generation counter are all exercised with
// glyphs of known size and no font, no GPU and no platform involved. Against a
// real font these would be assertions about whatever Menlo happens to do on the
// machine running them.

using namespace nano;
using namespace eacp;
using namespace eacp::Text;

namespace
{
// Produces rectangles of a size the test dictates, and counts how often it is
// asked — the cache is only believable if we can see the misses.
struct StubSource final : GlyphSource
{
    FontMetrics metrics(FontStyle style) const override
    {
        ++metricCalls;

        auto result = FontMetrics {};
        result.ascent = 20.f;
        result.descent = 6.f;
        result.leading = 2.f;
        result.advance = isBold(style) ? 12.f : 10.f;

        return result;
    }

    float scale() const override { return scaleValue; }

    GlyphBitmap rasterize(char32_t codepoint, FontStyle style) const override
    {
        ++rasterCalls;
        lastCodepoint = codepoint;
        lastStyle = style;

        auto bitmap = GlyphBitmap {};

        if (codepoint == U'\0')
            return bitmap; // invalid: no face can draw it

        bitmap.valid = true;
        bitmap.advance = 10.f;
        bitmap.bearingX = 1.f;
        bitmap.bearingY = 16.f;

        if (codepoint == U' ')
            return bitmap; // valid, but nothing to draw

        bitmap.format = codepoint >= 0x1F600 ? GlyphFormat::Color : GlyphFormat::Mask;
        bitmap.width = glyphWidth;
        bitmap.height = glyphHeight;
        bitmap.pixels.assign(
            (std::size_t) glyphWidth * glyphHeight * bytesPerPixel(bitmap.format),
            fillByte);

        return bitmap;
    }

    int glyphWidth = 8;
    int glyphHeight = 12;
    std::uint8_t fillByte = 0xff;
    float scaleValue = 1.f;

    mutable int rasterCalls = 0;
    mutable int metricCalls = 0;
    mutable char32_t lastCodepoint = 0;
    mutable FontStyle lastStyle = FontStyle::Regular;
};

// The atlas owns its source, so tests keep a raw pointer to inspect it.
struct Harness
{
    explicit Harness(int initialSize = 128, int maxSize = 512)
    {
        auto owned = makeOwned<StubSource>();
        source = owned.get();
        atlas = makeOwned<GlyphAtlas>(
            OwningPointer<GlyphSource> {std::move(owned)}, initialSize, maxSize);
    }

    StubSource* source = nullptr;
    OwningPointer<GlyphAtlas> atlas;
};
} // namespace

auto tRasterizesOnFirstRequest = test("GlyphAtlas/rasterizesOnFirstRequest") = []
{
    auto harness = Harness {};

    const auto slot = harness.atlas->glyph(U'A', FontStyle::Regular);

    check(slot.valid);
    check(!slot.empty);
    check(harness.source->rasterCalls == 1);
    check(harness.source->lastCodepoint == U'A');
};

// The cache is the reason this class exists: a glyph is rasterized once however
// often it appears on screen.
auto tCachesAcrossRequests = test("GlyphAtlas/rasterizesEachGlyphOnlyOnce") = []
{
    auto harness = Harness {};

    for (auto i = 0; i < 20; ++i)
        harness.atlas->glyph(U'A', FontStyle::Regular);

    check(harness.source->rasterCalls == 1);
};

// Style is part of the key, so bold A and regular A are different entries.
auto tStyleIsPartOfTheKey = test("GlyphAtlas/stylesAreCachedSeparately") = []
{
    auto harness = Harness {};

    const auto regular = harness.atlas->glyph(U'A', FontStyle::Regular);
    const auto bold = harness.atlas->glyph(U'A', FontStyle::Bold);
    const auto italic = harness.atlas->glyph(U'A', FontStyle::Italic);
    const auto boldItalic = harness.atlas->glyph(U'A', FontStyle::BoldItalic);

    check(harness.source->rasterCalls == 4);

    // Four distinct places in the atlas.
    check(regular.src.x != bold.src.x || regular.src.y != bold.src.y);
    check(italic.src.x != boldItalic.src.x || italic.src.y != boldItalic.src.y);
};

auto tReportsInvalidGlyphs = test("GlyphAtlas/reportsGlyphsNoFaceCanDraw") = []
{
    auto harness = Harness {};

    const auto slot = harness.atlas->glyph(U'\0', FontStyle::Regular);

    check(!slot.valid);
};

// A space advances the pen and draws nothing. Caching it as a valid-but-empty
// slot keeps it out of the rasterizer on every subsequent space.
auto tEmptyGlyphsAdvanceWithoutDrawing = test("GlyphAtlas/emptyGlyphAdvancesButDrawsNothing") = []
{
    auto harness = Harness {};

    const auto slot = harness.atlas->glyph(U' ', FontStyle::Regular);

    check(slot.valid);
    check(slot.empty);
    check(slot.advance > 0.f);
    check(slot.src.w == 0.f);

    harness.atlas->glyph(U' ', FontStyle::Regular);
    check(harness.source->rasterCalls == 1);
};

// Mask and colour glyphs go to different textures, so they cannot be packed on
// top of each other. Both starting at the same origin is the tell that they are
// in separate pages.
auto tColorAndMaskUseSeparatePages = test("GlyphAtlas/colorAndMaskGlyphsUseSeparatePages") = []
{
    auto harness = Harness {};

    const auto mask = harness.atlas->glyph(U'A', FontStyle::Regular);
    const auto color = harness.atlas->glyph(U'\U0001F600', FontStyle::Regular);

    check(mask.format == GlyphFormat::Mask);
    check(color.format == GlyphFormat::Color);
    check(mask.src.x == color.src.x);
    check(mask.src.y == color.src.y);
};

// Bitmap metrics are in device pixels; slots are in points. At scale 2 a glyph
// covers half as many points as pixels, which is what keeps layout
// resolution-independent.
auto tConvertsMetricsToPoints = test("GlyphAtlas/convertsPixelMetricsToPoints") = []
{
    auto harness = Harness {};
    harness.source->scaleValue = 2.f;

    const auto slot = harness.atlas->glyph(U'A', FontStyle::Regular);

    check(slot.advance == 5.f);       // 10 px / 2
    check(slot.offset.x == 0.5f);     // bearingX 1 px / 2

    // bearingY is measured up from the baseline; the offset is measured down to
    // the bitmap's top edge, so the sign flips.
    check(slot.offset.y == -8.f);     // -(16 px / 2)

    const auto metrics = harness.atlas->metrics(FontStyle::Regular);

    check(metrics.ascent == 10.f);
    check(metrics.descent == 3.f);
    check(metrics.advance == 5.f);
    check(metrics.lineHeight() == 14.f);
};

auto tMetricsFollowStyle = test("GlyphAtlas/metricsAreReportedPerStyle") = []
{
    auto harness = Harness {};

    check(harness.atlas->metrics(FontStyle::Regular).advance == 10.f);
    check(harness.atlas->metrics(FontStyle::Bold).advance == 12.f);
};

// The source rect must match the bitmap the source produced, or the shader
// samples the wrong texels.
auto tSourceRectMatchesBitmap = test("GlyphAtlas/sourceRectMatchesTheBitmapSize") = []
{
    auto harness = Harness {};
    harness.source->glyphWidth = 9;
    harness.source->glyphHeight = 17;

    const auto slot = harness.atlas->glyph(U'A', FontStyle::Regular);

    check(slot.src.w == 9.f);
    check(slot.src.h == 17.f);
};

// Growth, not eviction: filling past the initial size must keep every glyph
// already handed out, at the same coordinates, without re-rasterizing.
auto tGrowsWithoutLosingGlyphs = test("GlyphAtlas/growsRatherThanEvicting") = []
{
    auto harness = Harness {64, 1024};

    const auto first = harness.atlas->glyph(U'A', FontStyle::Regular);
    const auto startingSize = harness.atlas->size();
    const auto startingGeneration = harness.atlas->generation();

    // Enough distinct glyphs to overflow a 64px atlas several times over.
    for (char32_t cp = U'B'; cp < U'B' + 200; ++cp)
        harness.atlas->glyph(cp, FontStyle::Regular);

    check(harness.atlas->size() > startingSize);
    check(harness.atlas->generation() == startingGeneration);

    // The first glyph is untouched: same slot, still cached.
    const auto rasterCallsBefore = harness.source->rasterCalls;
    const auto again = harness.atlas->glyph(U'A', FontStyle::Regular);

    check(harness.source->rasterCalls == rasterCallsBefore);
    check(again.src.x == first.src.x);
    check(again.src.y == first.src.y);
};

auto tStopsGrowingAtMaxSize = test("GlyphAtlas/neverGrowsPastTheCap") = []
{
    auto harness = Harness {64, 128};

    for (char32_t cp = U'A'; cp < U'A' + 400; ++cp)
        harness.atlas->glyph(cp, FontStyle::Regular);

    check(harness.atlas->size() <= 128);
};

// At the cap the atlas clears, and the generation is how a caller finds out
// that slots it held are no longer valid.
auto tGenerationTicksOnReset = test("GlyphAtlas/generationTicksWhenTheAtlasIsCleared") = []
{
    auto harness = Harness {64, 64}; // no room to grow

    const auto startingGeneration = harness.atlas->generation();

    for (char32_t cp = U'A'; cp < U'A' + 400; ++cp)
        harness.atlas->glyph(cp, FontStyle::Regular);

    check(harness.atlas->generation() > startingGeneration);
    check(harness.atlas->size() == 64);
};

// After a clear the cache is genuinely empty, so a glyph requested again is
// rasterized again rather than returning a stale rect into freed space.
auto tResetDropsTheCache = test("GlyphAtlas/clearingDropsCachedSlots") = []
{
    auto harness = Harness {64, 64};

    harness.atlas->glyph(U'A', FontStyle::Regular);

    for (char32_t cp = U'B'; cp < U'B' + 400; ++cp)
        harness.atlas->glyph(cp, FontStyle::Regular);

    check(harness.atlas->generation() > 0);

    const auto before = harness.source->rasterCalls;
    harness.atlas->glyph(U'A', FontStyle::Regular);

    check(harness.source->rasterCalls == before + 1);
};

// A glyph too large for even a full-size atlas fails cleanly instead of
// looping or writing out of bounds.
auto tRejectsGlyphsLargerThanTheAtlas = test("GlyphAtlas/rejectsGlyphsBiggerThanTheAtlas") = []
{
    auto harness = Harness {64, 64};
    harness.source->glyphWidth = 400;
    harness.source->glyphHeight = 400;

    const auto slot = harness.atlas->glyph(U'A', FontStyle::Regular);

    check(!slot.valid);
};
