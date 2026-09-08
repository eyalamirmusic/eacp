#include "Common.h"

#include <eacp/Core/Utils/Environment.h>
#include <eacp/Core/Utils/Strings.h>

#include <initializer_list>
#include <string>

// What the paragraph-level bidi pass does once the Linux rasterizer shapes
// through it: a mixed Hebrew, Arabic and Latin line comes back with its
// glyphs placed in visual order rather than in the order the bytes were
// written. CoreText and DirectWrite do this inside themselves, so this half
// is Linux-only, as the emoji-presentation face test is. Self-skips when
// DejaVu Sans is not installed, and EACP_REQUIRE_FONTS=1 makes that a failure.

using namespace nano;
using namespace eacp;
using namespace eacp::Text;

namespace
{
constexpr char32_t alef = 0x05D0;
constexpr char32_t bet = 0x05D1;
constexpr char32_t gimel = 0x05D2;
constexpr char32_t arabicAlef = 0x0627;
constexpr char32_t arabicBeh = 0x0628;

constexpr const char* bidiFamily()
{
    return "DejaVu Sans";
}

FontRequest bidiRequest()
{
    auto request = FontRequest {};
    request.family = bidiFamily();
    request.pointSize = 24.f;
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
    const auto resolved =
        rasterizer.isValid()
        && Strings::equalsCaseInsensitive(rasterizer.resolvedFamily(), bidiFamily());

    check(resolved || getEnvValue("EACP_REQUIRE_FONTS") != "1",
          "EACP_REQUIRE_FONTS=1 but the family this test needs did not resolve, "
          "so the bidi reordering was never exercised");

    return resolved;
}

// Where the glyph that came from `byte` sits along the line. A cluster is a
// byte offset in the text that was shaped, which is how a visual position is
// tied back to the character that produced it.
float positionOfByte(const ShapedRun& run, int byte)
{
    for (const auto& glyph: run.glyphs)
        if (glyph.cluster == byte)
            return glyph.x;

    return -1.f;
}
} // namespace

auto tBidiOrdersHebrewRightToLeft = test("Text/bidiShapesInVisualOrder") = []
{
    const auto rasterizer = GlyphRasterizer {bidiRequest()};

    if (!has(rasterizer))
        return;

    // Three Hebrew letters on their own: the first one typed is drawn
    // rightmost, which is the whole point.
    const auto hebrew = utf8Of({alef, bet, gimel});
    const auto run = rasterizer.shape(hebrew, {});

    check(run.glyphs.size() == 3);
    check(run.advance > 0.f);

    check(positionOfByte(run, 0) > positionOfByte(run, 2));
    check(positionOfByte(run, 2) > positionOfByte(run, 4));
};

auto tBidiOrdersMixedText = test("Text/bidiShapesMixedDirectionInVisualOrder") = []
{
    const auto rasterizer = GlyphRasterizer {bidiRequest()};

    if (!has(rasterizer))
        return;

    // "ab" then a space then two Hebrew letters: a left-to-right paragraph,
    // so the Latin stays where it was written and only the Hebrew flips.
    const auto text = utf8Of({U'a', U'b', U' ', alef, bet});
    const auto run = rasterizer.shape(text, {});

    check(run.glyphs.size() == 5);

    check(positionOfByte(run, 0) < positionOfByte(run, 1));
    check(positionOfByte(run, 1) < positionOfByte(run, 2));

    // The Latin is left of the Hebrew, and within the Hebrew the first letter
    // typed is the rightmost.
    check(positionOfByte(run, 2) < positionOfByte(run, 5));
    check(positionOfByte(run, 3) > positionOfByte(run, 5));
};

auto tBidiKeepsDigitsLeftToRight = test("Text/bidiKeepsDigitsLeftToRight") = []
{
    const auto rasterizer = GlyphRasterizer {bidiRequest()};

    if (!has(rasterizer))
        return;

    // Two Arabic letters, a space and "12". The paragraph is right-to-left,
    // so the digits are drawn leftmost - and, being at an even level of their
    // own, they read left to right rather than as "21".
    const auto text = utf8Of({arabicAlef, arabicBeh, U' ', U'1', U'2'});
    const auto run = rasterizer.shape(text, {});

    const auto first = positionOfByte(run, 5);
    const auto second = positionOfByte(run, 6);

    check(first >= 0.f && second >= 0.f);
    check(first < second);

    // And the Arabic sits to the right of them.
    check(positionOfByte(run, 0) > second);
};

auto tBidiOrdersThreeScripts = test("Text/bidiShapesThreeScriptsInVisualOrder") = []
{
    const auto rasterizer = GlyphRasterizer {bidiRequest()};

    if (!has(rasterizer))
        return;

    // Hebrew, then Latin, then Arabic, in a right-to-left paragraph: the
    // Arabic is drawn leftmost, the Hebrew rightmost, and the Latin island
    // between them keeps its own direction.
    const auto text =
        utf8Of({alef, bet, U' ', U'a', U'b', U' ', arabicAlef, arabicBeh});
    const auto run = rasterizer.shape(text, {});

    const auto hebrew = positionOfByte(run, 0);
    const auto latin = positionOfByte(run, 5);
    const auto arabic = positionOfByte(run, 8);

    check(hebrew >= 0.f && latin >= 0.f && arabic >= 0.f);
    check(arabic < latin);
    check(latin < hebrew);

    // Latin inside a right-to-left paragraph still reads left to right.
    check(positionOfByte(run, 5) < positionOfByte(run, 6));
};

auto tBidiLeavesLatinAlone = test("Text/bidiLeavesLatinAlone") = []
{
    const auto rasterizer = GlyphRasterizer {bidiRequest()};

    if (!has(rasterizer))
        return;

    // The fast path: nothing right-to-left, so the run is what it always was.
    const auto run = rasterizer.shape("Latin 123", {});

    check(run.glyphs.size() == 9);

    for (auto index = 1; index < run.glyphs.size(); ++index)
        check(run.glyphs[index].x > run.glyphs[index - 1].x);
};
