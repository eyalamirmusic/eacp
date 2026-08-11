#include "Common.h"

using namespace nano;
using namespace eacp;
using namespace eacp::Text;

namespace
{
// Counts how often it is asked, so cache misses are visible.
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

        // Lets a test tell two faces apart by what they measure.
        result.advance *= pointSize / 13.f;

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

        bitmap.format =
            codepoint >= 0x1F600 ? GlyphFormat::Color : GlyphFormat::Mask;
        bitmap.width = glyphWidth;
        bitmap.height = glyphHeight;
        bitmap.pixels.assign((std::size_t) glyphWidth * glyphHeight
                                 * bytesPerPixel(bitmap.format),
                             fillByte);

        return bitmap;
    }

    int glyphWidth = 8;
    int glyphHeight = 12;
    std::uint8_t fillByte = 0xff;
    float scaleValue = 1.f;
    float pointSize = 13.f;

    mutable int rasterCalls = 0;
    mutable int metricCalls = 0;
    mutable char32_t lastCodepoint = 0;
    mutable FontStyle lastStyle = FontStyle::Regular;
};

struct Harness
{
    explicit Harness(int initialSize = 128, int maxSize = 512)
    {
        auto request = FontRequest {};
        request.family = "stub";
        request.pointSize = 13.f;
        request.scale = 1.f;

        atlas = makeOwned<GlyphAtlas>([this](const FontRequest& faceRequest)
                                      { return make(faceRequest); },
                                      request,
                                      initialSize,
                                      maxSize);
    }

    OwningPointer<GlyphSource> make(const FontRequest& request)
    {
        auto stub = makeOwned<StubSource>();

        stub->scaleValue = request.scale;
        stub->pointSize = request.pointSize;

        sources.add(stub.get());

        return OwningPointer<GlyphSource> {std::move(stub)};
    }

    // Face 0 is the default face.
    StubSource* source(int index = 0) const { return sources[index]; }

    int sourceCount() const { return sources.size(); }

    Vector<StubSource*> sources;
    OwningPointer<GlyphAtlas> atlas;
};
} // namespace

auto tRasterizesOnFirstRequest = test("GlyphAtlas/rasterizesOnFirstRequest") = []
{
    auto harness = Harness {};

    const auto slot = harness.atlas->glyph(U'A', FontStyle::Regular);

    check(slot.valid);
    check(!slot.empty);
    check(harness.source()->rasterCalls == 1);
    check(harness.source()->lastCodepoint == U'A');
};

auto tCachesAcrossRequests = test("GlyphAtlas/rasterizesEachGlyphOnlyOnce") = []
{
    auto harness = Harness {};

    for (auto i = 0; i < 20; ++i)
        harness.atlas->glyph(U'A', FontStyle::Regular);

    check(harness.source()->rasterCalls == 1);
};

auto tStyleIsPartOfTheKey = test("GlyphAtlas/stylesAreCachedSeparately") = []
{
    auto harness = Harness {};

    const auto regular = harness.atlas->glyph(U'A', FontStyle::Regular);
    const auto bold = harness.atlas->glyph(U'A', FontStyle::Bold);
    const auto italic = harness.atlas->glyph(U'A', FontStyle::Italic);
    const auto boldItalic = harness.atlas->glyph(U'A', FontStyle::BoldItalic);

    check(harness.source()->rasterCalls == 4);

    check(regular.src.x != bold.src.x || regular.src.y != bold.src.y);
    check(italic.src.x != boldItalic.src.x || italic.src.y != boldItalic.src.y);
};

auto tReportsInvalidGlyphs = test("GlyphAtlas/reportsGlyphsNoFaceCanDraw") = []
{
    auto harness = Harness {};

    const auto slot = harness.atlas->glyph(U'\0', FontStyle::Regular);

    check(!slot.valid);
};

auto tEmptyGlyphsAdvanceWithoutDrawing =
    test("GlyphAtlas/emptyGlyphAdvancesButDrawsNothing") = []
{
    auto harness = Harness {};

    const auto slot = harness.atlas->glyph(U' ', FontStyle::Regular);

    check(slot.valid);
    check(slot.empty);
    check(slot.advance > 0.f);
    check(slot.src.w == 0.f);

    harness.atlas->glyph(U' ', FontStyle::Regular);
    check(harness.source()->rasterCalls == 1);
};

auto tColorAndMaskUseSeparatePages =
    test("GlyphAtlas/colorAndMaskGlyphsUseSeparatePages") = []
{
    auto harness = Harness {};

    const auto mask = harness.atlas->glyph(U'A', FontStyle::Regular);
    const auto color = harness.atlas->glyph(U'\U0001F600', FontStyle::Regular);

    check(mask.format == GlyphFormat::Mask);
    check(color.format == GlyphFormat::Color);
    check(mask.src.x == color.src.x);
    check(mask.src.y == color.src.y);
};

auto tConvertsMetricsToPoints = test("GlyphAtlas/convertsPixelMetricsToPoints") = []
{
    auto harness = Harness {};

    harness.atlas->setScale(2.f);

    const auto slot = harness.atlas->glyph(U'A', FontStyle::Regular);

    check(slot.advance == 5.f); // 10 px / 2
    check(slot.offset.x == 0.5f); // bearingX 1 px / 2

    // bearingY measures up from the baseline, the offset down, so the sign flips.
    check(slot.offset.y == -8.f); // -(16 px / 2)

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

auto tSourceRectMatchesBitmap =
    test("GlyphAtlas/sourceRectMatchesTheBitmapSize") = []
{
    auto harness = Harness {};
    harness.source()->glyphWidth = 9;
    harness.source()->glyphHeight = 17;

    const auto slot = harness.atlas->glyph(U'A', FontStyle::Regular);

    check(slot.src.w == 9.f);
    check(slot.src.h == 17.f);
};

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

    const auto rasterCallsBefore = harness.source()->rasterCalls;
    const auto again = harness.atlas->glyph(U'A', FontStyle::Regular);

    check(harness.source()->rasterCalls == rasterCallsBefore);
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

auto tGenerationTicksOnReset =
    test("GlyphAtlas/generationTicksWhenTheAtlasIsCleared") = []
{
    auto harness = Harness {64, 64}; // no room to grow

    const auto startingGeneration = harness.atlas->generation();

    for (char32_t cp = U'A'; cp < U'A' + 400; ++cp)
        harness.atlas->glyph(cp, FontStyle::Regular);

    check(harness.atlas->generation() > startingGeneration);
    check(harness.atlas->size() == 64);
};

auto tResetDropsTheCache = test("GlyphAtlas/clearingDropsCachedSlots") = []
{
    auto harness = Harness {64, 64};

    harness.atlas->glyph(U'A', FontStyle::Regular);

    for (char32_t cp = U'B'; cp < U'B' + 400; ++cp)
        harness.atlas->glyph(cp, FontStyle::Regular);

    check(harness.atlas->generation() > 0);

    const auto before = harness.source()->rasterCalls;
    harness.atlas->glyph(U'A', FontStyle::Regular);

    check(harness.source()->rasterCalls == before + 1);
};

auto tRejectsGlyphsLargerThanTheAtlas =
    test("GlyphAtlas/rejectsGlyphsBiggerThanTheAtlas") = []
{
    auto harness = Harness {64, 64};
    harness.source()->glyphWidth = 400;
    harness.source()->glyphHeight = 400;

    const auto slot = harness.atlas->glyph(U'A', FontStyle::Regular);

    check(!slot.valid);
};

auto tFaceIsPartOfTheKey = test("GlyphAtlas/facesAreCachedSeparately") = []
{
    auto harness = Harness {};

    const auto small = harness.atlas->findOrAddFace("stub", 13.f);
    const auto large = harness.atlas->findOrAddFace("stub", 24.f);

    check(small != large);

    const auto a = harness.atlas->glyph(U'A', FontStyle::Regular, small);
    const auto b = harness.atlas->glyph(U'A', FontStyle::Regular, large);

    check(a.valid);
    check(b.valid);
    check(a.src.x != b.src.x || a.src.y != b.src.y);

    check(harness.source(0)->rasterCalls == 1);
    check(harness.source(1)->rasterCalls == 1);
};

auto tReusesAFaceItAlreadyHas =
    test("GlyphAtlas/reusesAFaceRatherThanBuildingItTwice") = []
{
    auto harness = Harness {};

    const auto first = harness.atlas->findOrAddFace("stub", 24.f);
    const auto again = harness.atlas->findOrAddFace("stub", 24.f);

    check(first == again);
    check(harness.atlas->faceCount() == 2); // the default face, and this one
    check(harness.sourceCount() == 2);
};

auto tNearlyEqualSizesShareAFace =
    test("GlyphAtlas/sizesWithinToleranceShareAFace") = []
{
    auto harness = Harness {};

    const auto face = harness.atlas->findOrAddFace("stub", 24.f);

    check(harness.atlas->findOrAddFace("stub", 24.001f) == face);
    check(harness.atlas->findOrAddFace("stub", 24.5f) != face);
};

auto tFamilyIsPartOfAFace = test("GlyphAtlas/familiesAreSeparateFaces") = []
{
    auto harness = Harness {};

    check(harness.atlas->findOrAddFace("stub", 13.f) == 0);
    check(harness.atlas->findOrAddFace("other", 13.f) != 0);
};

auto tScaleChangeKeepsFaceIndices =
    test("GlyphAtlas/aScaleChangeRebuildsFacesInPlace") = []
{
    auto harness = Harness {};

    const auto large = harness.atlas->findOrAddFace("stub", 24.f);
    harness.atlas->glyph(U'A', FontStyle::Regular, large);

    const auto generation = harness.atlas->generation();

    harness.atlas->setScale(2.f);

    check(harness.atlas->scale() == 2.f);
    check(harness.atlas->faceCount() == 2);

    check(harness.sourceCount() == 4);
    check(harness.source(2)->scaleValue == 2.f);
    check(harness.source(3)->scaleValue == 2.f);

    check(harness.atlas->generation() > generation);

    const auto slot = harness.atlas->glyph(U'A', FontStyle::Regular, large);

    check(slot.valid);
    check(harness.source(3)->rasterCalls == 1);
};

auto tUnchangedScaleCostsNothing =
    test("GlyphAtlas/settingTheSameScaleChangesNothing") = []
{
    auto harness = Harness {};

    harness.atlas->glyph(U'A', FontStyle::Regular);
    const auto generation = harness.atlas->generation();

    harness.atlas->setScale(1.f);

    check(harness.atlas->generation() == generation);
    check(harness.sourceCount() == 1);
    check(harness.source()->rasterCalls == 1);
};

auto tRejectsUnknownFaces = test("GlyphAtlas/anUnknownFaceDrawsNothing") = []
{
    auto harness = Harness {};

    check(!harness.atlas->glyph(U'A', FontStyle::Regular, 7).valid);
    check(!harness.atlas->glyph(U'A', FontStyle::Regular, -1).valid);
    check(harness.atlas->metrics(FontStyle::Regular, 7).lineHeight() == 0.f);
};
