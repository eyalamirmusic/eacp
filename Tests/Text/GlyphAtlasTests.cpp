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
    FontMetrics metrics(const FontVariant& variant) const override
    {
        ++metricCalls;

        auto result = FontMetrics {};
        result.ascent = 20.f;
        result.descent = 6.f;
        result.leading = 2.f;
        result.advance = variant.weight >= 600 ? 12.f : 10.f;

        // The size the face was built at, so a test can tell two faces apart by
        // what they measure rather than by identity alone.
        result.advance *= pointSize / 13.f;

        return result;
    }

    float scale() const override { return scaleValue; }

    // One glyph per codepoint, its id the codepoint itself and every advance
    // ten pixels; a CJK codepoint comes from font 1, the way a fallback face
    // would, so a test can see the font in the key.
    ShapedRun shape(std::string_view text, const FontVariant&) const override
    {
        ++shapeCalls;

        auto run = ShapedRun {};
        auto index = std::size_t {0};

        while (index < text.size())
        {
            const auto start = index;
            const auto codepoint = decodeUtf8(text, index);
            const auto font = codepoint >= 0x4E00 && codepoint < 0xA000 ? 1 : 0;

            run.glyphs.add({{codepoint, font}, run.advance, 0.f, (int) start});
            run.advance += 10.f;
        }

        return run;
    }

    GlyphBitmap rasterize(GlyphKey key, const FontVariant& variant) const override
    {
        ++rasterCalls;
        lastCodepoint = key.glyph;
        lastStyle = styleOf(variant);
        lastFont = key.font;

        const auto codepoint = (char32_t) key.glyph;
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
    mutable int shapeCalls = 0;
    mutable int metricCalls = 0;
    mutable char32_t lastCodepoint = 0;
    mutable FontStyle lastStyle = FontStyle::Regular;
    mutable int lastFont = 0;
};

// The atlas owns its faces, so the harness records every stub it hands over and
// keeps a raw pointer to each — which is also how a test sees how many faces
// were actually built rather than merely asked for.
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

    // The stub behind one face, in the order the atlas built them. Face 0 is
    // the default face, and is what a test that never asks for another is
    // drawing through.
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

// The cache is the reason this class exists: a glyph is rasterized once however
// often it appears on screen.
auto tCachesAcrossRequests = test("GlyphAtlas/rasterizesEachGlyphOnlyOnce") = []
{
    auto harness = Harness {};

    for (auto i = 0; i < 20; ++i)
        harness.atlas->glyph(U'A', FontStyle::Regular);

    check(harness.source()->rasterCalls == 1);
};

// Style is part of the key, so bold A and regular A are different entries.
auto tStyleIsPartOfTheKey = test("GlyphAtlas/stylesAreCachedSeparately") = []
{
    auto harness = Harness {};

    const auto regular = harness.atlas->glyph(U'A', FontStyle::Regular);
    const auto bold = harness.atlas->glyph(U'A', FontStyle::Bold);
    const auto italic = harness.atlas->glyph(U'A', FontStyle::Italic);
    const auto boldItalic = harness.atlas->glyph(U'A', FontStyle::BoldItalic);

    check(harness.source()->rasterCalls == 4);

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

// Mask and colour glyphs go to different textures, so they cannot be packed on
// top of each other. Both starting at the same origin is the tell that they are
// in separate pages.
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

// Bitmap metrics are in device pixels; slots are in points. At scale 2 a glyph
// covers half as many points as pixels, which is what keeps layout
// resolution-independent.
auto tConvertsMetricsToPoints = test("GlyphAtlas/convertsPixelMetricsToPoints") = []
{
    auto harness = Harness {};

    // Through the atlas rather than by reaching into the stub, because the
    // scale is the atlas's: it is what every face is built at, and a face built
    // at another one would be mixing two displays in a single texture.
    harness.atlas->setScale(2.f);

    const auto slot = harness.atlas->glyph(U'A', FontStyle::Regular);

    check(slot.advance == 5.f); // 10 px / 2
    check(slot.offset.x == 0.5f); // bearingX 1 px / 2

    // bearingY is measured up from the baseline; the offset is measured down to
    // the bitmap's top edge, so the sign flips.
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

// The source rect must match the bitmap the source produced, or the shader
// samples the wrong texels.
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

// At the cap the atlas clears, and the generation is how a caller finds out
// that slots it held are no longer valid.
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

// After a clear the cache is genuinely empty, so a glyph requested again is
// rasterized again rather than returning a stale rect into freed space.
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

// A glyph too large for even a full-size atlas fails cleanly instead of
// looping or writing out of bounds.
auto tRejectsGlyphsLargerThanTheAtlas =
    test("GlyphAtlas/rejectsGlyphsBiggerThanTheAtlas") = []
{
    auto harness = Harness {64, 64};
    harness.source()->glyphWidth = 400;
    harness.source()->glyphHeight = 400;

    const auto slot = harness.atlas->glyph(U'A', FontStyle::Regular);

    check(!slot.valid);
};

// The face is part of the key, which is the whole point of the table: the same
// character at two sizes is two entries in one atlas rather than two atlases.
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

    // One raster per (face, glyph), and the second face went to its own source.
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

// A drag that scales a document asks for a slightly different size every frame.
// Matching within a tolerance is what keeps that from costing a face and a
// full set of glyphs per frame of the drag.
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

// A caller holds face indices across frames, so a scale change has to rebuild
// what is behind them rather than renumber them.
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

    // Both faces were built again, at the new scale.
    check(harness.sourceCount() == 4);
    check(harness.source(2)->scaleValue == 2.f);
    check(harness.source(3)->scaleValue == 2.f);

    // Everything cached was rasterized for the old display, so it all goes --
    // and the generation says so, the same way a full atlas does.
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

// A face nobody built is a caller's mistake, and reading past the table would
// be the expensive kind. It draws nothing instead.
auto tRejectsUnknownFaces = test("GlyphAtlas/anUnknownFaceDrawsNothing") = []
{
    auto harness = Harness {};

    check(!harness.atlas->glyph(U'A', FontStyle::Regular, 7).valid);
    check(!harness.atlas->glyph(U'A', FontStyle::Regular, -1).valid);
    check(harness.atlas->metrics(FontStyle::Regular, 7).lineHeight() == 0.f);
};

// A shaped string comes back as one slot per glyph the source placed, each
// with its pen and the byte it came from, and rasterizes each glyph once.
auto tShapeReturnsASlotPerGlyph = test("GlyphAtlas/shapeReturnsASlotPerGlyph") = []
{
    auto harness = Harness {};

    const auto shaped = harness.atlas->shape("ab", {});

    check(shaped.glyphs.size() == 2);
    check(shaped.advance == 20.f);
    check(shaped.glyphs[0].pen.x == 0.f);
    check(shaped.glyphs[1].pen.x == 10.f);
    check(shaped.glyphs[0].cluster == 0);
    check(shaped.glyphs[1].cluster == 1);
    check(shaped.glyphs[0].slot.valid && shaped.glyphs[1].slot.valid);
    check(harness.source()->rasterCalls == 2);
};

// A string is shaped once however often it is measured or drawn - a document
// asks for the same words over and over - and the cache goes with the slots.
auto tShapedStringsAreCached = test("GlyphAtlas/shapedStringsAreCached") = []
{
    auto harness = Harness {};

    harness.atlas->shape("hello", {});
    harness.atlas->shape("hello", {});
    harness.atlas->shape("hello", {});

    check(harness.source()->shapeCalls == 1);
    check(harness.source()->rasterCalls == 4); // h, e, l, o

    // Another variant is another string.
    harness.atlas->shape("hello", {700, false});
    check(harness.source()->shapeCalls == 2);

    // A scale change rebuilds the face and drops every slot, and the shaped
    // strings holding them go too: the string is shaped again, by the new
    // source.
    harness.atlas->setScale(2.f);
    harness.atlas->shape("hello", {});
    check(harness.sourceCount() == 2);
    check(harness.source(1)->shapeCalls == 1);
    check(harness.source(1)->rasterCalls == 4);
};

// A glyph id names a glyph in one font. The same id in a fallback font is
// another glyph, and the key knows it.
auto tFontIsPartOfTheKey = test("GlyphAtlas/fontIsPartOfTheKey") = []
{
    auto harness = Harness {};

    const auto own = harness.atlas->glyph(GlyphKey {0x41, 0}, {});
    const auto fallback = harness.atlas->glyph(GlyphKey {0x41, 1}, {});

    check(harness.source()->rasterCalls == 2);
    check(harness.source()->lastFont == 1);
    check(own.src.x != fallback.src.x || own.src.y != fallback.src.y);

    // And a shaped string that reaches into the fallback font keys it there.
    const auto shaped = harness.atlas->shape("a\xe6\xbc\xa2", {});
    check(shaped.glyphs.size() == 2);
    check(harness.source()->lastFont == 1);
};

// The nine weights are nine faces of the key; a face asked for as bold and
// as 700 is one.
auto tWeightIsPartOfTheKey = test("GlyphAtlas/weightsAreCachedSeparately") = []
{
    auto harness = Harness {};

    harness.atlas->glyph(GlyphKey {0x41, 0}, {300, false});
    harness.atlas->glyph(GlyphKey {0x41, 0}, {400, false});
    harness.atlas->glyph(GlyphKey {0x41, 0}, {700, false});

    check(harness.source()->rasterCalls == 3);

    harness.atlas->glyph(U'A', FontStyle::Bold);
    check(harness.source()->rasterCalls == 3, "bold is the 700 face");

    harness.atlas->glyph(GlyphKey {0x41, 0}, {650, false});
    check(harness.source()->rasterCalls == 3, "650 rounds to the 700 face");
};
