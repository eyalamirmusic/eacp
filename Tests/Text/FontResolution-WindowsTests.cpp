#include "Common.h"

#include <eacp/Graphics/D2D-Windows.h>
#include <eacp/Graphics/Primitives/Font.h>
#include <eacp/Graphics/Primitives/TextMetrics.h>

#include <fstream>
#include <iterator>
#include <vector>

// The shared font registry (Graphics/Primitives/FontRegistry-Windows.cpp) —
// the Windows stand-in for CoreText's process-wide font registry.
//
// The bug these pin down: a font registered from memory was visible only to
// the eacp-text rasterizer (Graphics::Font resolved against the system
// collection), and a PostScript name — the spelling CTFontCreateWithName
// accepts, so the one cross-platform callers ship — resolved on Apple but
// silently substituted a fallback family on Windows.
//
// Windows guarantees Arial, whose PostScript name (ArialMT) usefully differs
// from its family name, so these run against it rather than shipping a font.

using namespace nano;
using namespace eacp;
using namespace eacp::Graphics;

namespace
{
std::vector<unsigned char> readFile(const wchar_t* path)
{
    auto stream = std::ifstream(path, std::ios::binary);

    return {std::istreambuf_iterator<char> {stream},
            std::istreambuf_iterator<char> {}};
}
} // namespace

auto tPostScriptNameResolves =
    test("FontResolution/postScriptNameResolvesToItsFamily") = []
{
    check(resolveFontFamilyName(L"Arial") == L"Arial");
    check(resolveFontFamilyName(L"ArialMT") == L"Arial");
    check(resolveFontFamilyName(L"NoSuchFace-Regular").empty());
};

// The rasterizer must land on the named face, not on a substitute — the same
// glyphs, so the same advances as asking by family name.
auto tRasterizerAcceptsAPostScriptName =
    test("FontResolution/rasterizerResolvesAPostScriptName") = []
{
    auto byFamily = Text::FontRequest {};
    byFamily.family = "Arial";
    byFamily.pointSize = 16.f;

    auto byPostScript = byFamily;
    byPostScript.family = "ArialMT";

    const auto family = Text::GlyphRasterizer {byFamily};
    const auto postScript = Text::GlyphRasterizer {byPostScript};

    if (!family.isValid())
        return;

    check(postScript.isValid());
    check(postScript.rasterize(U'W', Text::FontStyle::Regular).advance
          == family.rasterize(U'W', Text::FontStyle::Regular).advance);
};

// Graphics::Font is the path the bug hid in: it used to hand DirectWrite the
// raw name against the system collection, and an embedded or PostScript name
// silently became the fallback family.
auto tGraphicsFontResolvesAPostScriptName =
    test("FontResolution/graphicsFontResolvesAPostScriptName") = []
{
    const auto font = Font {FontOptions().withName("ArialMT").withSize(12.f)};
    auto* format = static_cast<IDWriteTextFormat*>(font.getHandle());

    if (format == nullptr)
        return;

    wchar_t family[64] = {};
    format->GetFontFamilyName(family, 64);

    check(std::wstring {family} == L"Arial");
};

// DWRITE_TEXT_METRICS.width stops at the last ink, so a lone space used to
// measure zero and letter-spaced captions fused their words. CTLine's
// typographic width counts trailing whitespace; Windows must agree.
auto tSpaceMeasuresItsAdvance = test("TextMetrics/aLoneSpaceMeasuresItsAdvance") = []
{
    const auto font = Font {FontOptions().withName("Arial").withSize(12.f)};

    if (font.getHandle() == nullptr)
        return;

    check(TextMetrics::measureWidth(" ", font) > 0.f);
    check(TextMetrics::measureWidth("a b", font)
          > TextMetrics::measureWidth("ab", font));
};

// Registration rebuilds the shared collection over every registered file plus
// the system set — a broken rebuild would drop the system fonts on the floor.
auto tMemoryFontRegistrationKeepsTheCollectionWhole =
    test("FontResolution/memoryFontRegistrationKeepsSystemFonts") = []
{
    wchar_t windows[MAX_PATH] = {};
    GetWindowsDirectoryW(windows, MAX_PATH);

    const auto bytes =
        readFile((std::wstring {windows} + L"\\Fonts\\arial.ttf").c_str());

    if (bytes.empty())
        return;

    const auto registered = registerMemoryFontData(bytes.data(), bytes.size());
    check(registered.has_value());

    if (registered)
    {
        check(registered->family == L"Arial");
        check(registered->postScriptName == L"ArialMT");
    }

    // The rebuilt collection still resolves everything it did before.
    check(resolveFontFamilyName(L"ArialMT") == L"Arial");
    check(!resolveFontFamilyName(L"Segoe UI").empty());
    check(Text::GlyphRasterizer {Text::FontRequest {.family = "Arial"}}.isValid());
};
