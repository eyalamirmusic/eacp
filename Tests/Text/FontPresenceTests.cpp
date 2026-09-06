#include "Common.h"

#include <eacp/Core/Utils/Environment.h>
#include <eacp/Core/Utils/Logging.h>
#include <eacp/Core/Utils/Strings.h>

#include <string>

using namespace nano;
using namespace eacp;
using namespace eacp::Text;

// The one test in this directory that does not self-skip, and the answer to
// the failure mode every other one has: a font test whose family did not
// resolve returns immediately and ctest scores it as a pass, so a CI lane
// whose font packages were never installed reports a full green Text suite
// that shaped nothing at all. The same shape as
// Tests/GPU/DevicePresenceTests.cpp, for the same reason.
//
// EACP_REQUIRE_FONTS=1 says the platform's stock faces are expected here. A
// lane that sets it and finds them missing fails, with what did resolve
// printed either way. Nothing sets it by default, so a bare container with no
// fonts still gets a green run.

namespace
{
// The proportional face the platform ships, beside the fixed-pitch one
// defaultMonospaceFamily() already names.
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

// What the platform resolved a family to, or nothing when it resolved to no
// face at all. A substitute counts as resolved and is reported as itself, so
// the check below can say the family was substituted rather than absent.
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

    // Han and a grinning face: the two the fallback chain has to reach for,
    // since no fixed-pitch Latin family carries either. Spelled as escapes
    // because the build does not force a UTF-8 source encoding.
    const auto han = rasterizer.rasterize(U'\u6f22', FontStyle::Regular);
    const auto emoji = rasterizer.rasterize(U'\U0001F600', FontStyle::Regular);

    // Printed before the checks, so a failing lane says what it found as well
    // as that it was not what it wanted.
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
