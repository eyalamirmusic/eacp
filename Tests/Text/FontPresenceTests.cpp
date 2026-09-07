#include "Common.h"

#include <eacp/Core/Utils/Environment.h>
#include <eacp/Core/Utils/Logging.h>
#include <eacp/Core/Utils/Strings.h>

#include <string>

using namespace nano;
using namespace eacp;
using namespace eacp::Text;

namespace
{
constexpr const char* stockProportionalFamily()
{
    if constexpr (Platform::isWindows())
        return "Arial";
    else if constexpr (Platform::isLinux())
        return "DejaVu Sans";
    else
        return "Helvetica";
}

FontRequest presenceRequest(const char* family)
{
    auto request = FontRequest {};
    request.family = family;
    request.pointSize = 24.f;
    request.scale = 1.f;

    return request;
}

// Empty when nothing resolved; a substitute counts as resolved, and is named.
std::string resolvedFamilyOf(const char* family)
{
    const auto rasterizer = GlyphRasterizer {presenceRequest(family)};

    return rasterizer.isValid() ? rasterizer.resolvedFamily() : std::string {};
}

std::string describe(const std::string& resolved)
{
    return resolved.empty() ? std::string {"(nothing)"} : resolved;
}

std::string describe(const GlyphBitmap& bitmap)
{
    if (!bitmap.valid)
        return "invalid";

    if (bitmap.isEmpty())
        return "empty";

    return bitmap.format == GlyphFormat::Color ? "colour" : "monochrome";
}
} // namespace

auto tFontsArePresentWhenRequired = test("Text/fontsArePresentWhenRequired") = []
{
    const auto monospace = resolvedFamilyOf(defaultMonospaceFamily());
    const auto proportional = resolvedFamilyOf(stockProportionalFamily());

    const auto rasterizer =
        GlyphRasterizer {presenceRequest(defaultMonospaceFamily())};

    // Han and a grinning face, neither of which a fixed-pitch Latin family
    // carries. Escaped because the build does not force a UTF-8 source encoding.
    const auto han = rasterizer.rasterize(U'\u6f22', FontStyle::Regular);
    const auto emoji = rasterizer.rasterize(U'\U0001F600', FontStyle::Regular);

    LOG("Text monospace family: ",
        defaultMonospaceFamily(),
        " -> ",
        describe(monospace));
    LOG("Text proportional family: ",
        stockProportionalFamily(),
        " -> ",
        describe(proportional));
    LOG("Text CJK glyph U+6F22: ", describe(han));
    LOG("Text emoji glyph U+1F600: ", describe(emoji));

    if (getEnvValue("EACP_REQUIRE_FONTS") != "1")
        return;

    check(Strings::equalsCaseInsensitive(monospace, defaultMonospaceFamily()),
          "EACP_REQUIRE_FONTS=1 but the platform's fixed-pitch family did not "
          "resolve to itself - every shaping and rasterizing test would have "
          "skipped and reported a pass");

    check(Strings::equalsCaseInsensitive(proportional, stockProportionalFamily()),
          "EACP_REQUIRE_FONTS=1 but the platform's proportional family did not "
          "resolve to itself - the kerning, ligature and weight tests would "
          "have skipped and reported a pass");

    check(han.valid && !han.isEmpty(),
          "EACP_REQUIRE_FONTS=1 but nothing on this machine draws Han, so "
          "fallback to another face was never exercised");

    check(emoji.valid && !emoji.isEmpty(),
          "EACP_REQUIRE_FONTS=1 but nothing on this machine draws an emoji");

    check(emoji.format == GlyphFormat::Color,
          "EACP_REQUIRE_FONTS=1 but the emoji came back as a mask, so the "
          "colour page of the atlas is never filled - install a colour emoji "
          "font (fonts-noto-color-emoji)");
};
