#include "Common.h"

#include <eacp/Core/Utils/Environment.h>
#include <eacp/Core/Utils/Strings.h>

#include <initializer_list>
#include <string>

// Which face the Linux rasterizer reaches for once Emoji_Presentation has
// spoken: a colour font for a default-emoji codepoint or one wearing U+FE0F,
// the requested family for everything else. CoreText and DirectWrite make that
// choice themselves, so this half is Linux-only. Self-skips when the family is
// not installed, and EACP_REQUIRE_FONTS=1 makes that skip a failure.

using namespace nano;
using namespace eacp;
using namespace eacp::Text;

namespace
{
constexpr char32_t grinningFace = 0x1F600;
constexpr char32_t regionalIndicatorA = 0x1F1E6;
constexpr char32_t heavyBlackHeart = 0x2764;
constexpr char32_t copyrightSign = 0x00A9;

constexpr const char* presentationFamily()
{
    return "DejaVu Sans";
}

FontRequest presentationRequest()
{
    auto request = FontRequest {};
    request.family = presentationFamily();
    request.pointSize = 32.f;
    request.scale = 1.f;

    return request;
}

std::string utf8Of(std::initializer_list<char32_t> codepoints)
{
    auto text = std::string {};

    for (const auto codepoint: codepoints)
    {
        char encoded[4] = {};

        text.append(encoded, (std::size_t) encodeUtf8(codepoint, encoded));
    }

    return text;
}

bool has(const GlyphRasterizer& rasterizer)
{
    const auto resolved = rasterizer.isValid()
                          && Strings::equalsCaseInsensitive(
                              rasterizer.resolvedFamily(), presentationFamily());

    check(resolved || getEnvValue("EACP_REQUIRE_FONTS") != "1",
          "EACP_REQUIRE_FONTS=1 but the family this test needs did not resolve, "
          "so the emoji-presentation rule was never exercised");

    return resolved;
}

GlyphBitmap firstGlyphOf(const GlyphRasterizer& rasterizer,
                         std::initializer_list<char32_t> codepoints)
{
    const auto run = rasterizer.shape(utf8Of(codepoints), {});

    if (run.glyphs.empty())
        return {};

    return rasterizer.rasterize(run.glyphs[0].key, {}, {});
}

int fontsUsedBy(const GlyphRasterizer& rasterizer,
                std::initializer_list<char32_t> codepoints)
{
    const auto run = rasterizer.shape(utf8Of(codepoints), {});
    auto fonts = Vector<int> {};

    for (const auto& glyph: run.glyphs)
    {
        auto seen = false;

        for (const auto font: fonts)
            seen = seen || font == glyph.key.font;

        if (!seen)
            fonts.add(glyph.key.font);
    }

    return fonts.size();
}
} // namespace

auto tPresentationChoosesTheFace = test("Text/emojiPresentationChoosesTheFace") = []
{
    const auto rasterizer = GlyphRasterizer {presentationRequest()};

    if (!has(rasterizer))
        return;

    const auto emoji = firstGlyphOf(rasterizer, {grinningFace});
    const auto regional = firstGlyphOf(rasterizer, {regionalIndicatorA});
    const auto heart = firstGlyphOf(rasterizer, {heavyBlackHeart});
    const auto heartAsEmoji =
        firstGlyphOf(rasterizer, {heavyBlackHeart, emojiVariationSelector});
    const auto copyright = firstGlyphOf(rasterizer, {copyrightSign});

    check(emoji.valid && emoji.format == GlyphFormat::Color);
    check(regional.valid && regional.format == GlyphFormat::Color);
    check(heartAsEmoji.valid && heartAsEmoji.format == GlyphFormat::Color);

    check(heart.valid && heart.format == GlyphFormat::Mask);
    check(copyright.valid && copyright.format == GlyphFormat::Mask);
};

auto tVariationSelectorStaysWithItsBase =
    test("Text/variationSelectorStaysWithItsBase") = []
{
    const auto rasterizer = GlyphRasterizer {presentationRequest()};

    if (!has(rasterizer))
        return;

    // A variation selector is a mark on the codepoint before it, so it
    // itemizes into that codepoint's item and that codepoint's font rather
    // than falling out to a fallback lookup of its own - which used to cost
    // a second run and a second face for a glyph nothing draws.
    check(fontsUsedBy(rasterizer, {heavyBlackHeart, emojiVariationSelector}) == 1);
    check(fontsUsedBy(rasterizer, {copyrightSign, emojiVariationSelector}) == 1);
    check(fontsUsedBy(rasterizer, {grinningFace, emojiVariationSelector}) == 1);

    // And the face it settled on is still the one the property asked for.
    const auto heart =
        firstGlyphOf(rasterizer, {heavyBlackHeart, emojiVariationSelector});

    check(heart.valid && heart.format == GlyphFormat::Color);
};
