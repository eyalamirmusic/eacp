#include "Common.h"

#include <eacp/Graphics/D2D-Windows.h>
#include <eacp/Graphics/Primitives/Font.h>
#include <eacp/Graphics/Primitives/TextMetrics.h>

#include <algorithm>
#include <cstdio>
#include <vector>

// Regression: a memory-registered font was visible only to the eacp-text
// rasterizer, and a PostScript name silently fell back to another family on
// Windows. Arial is guaranteed and its PostScript name (ArialMT) differs.

using namespace nano;
using namespace eacp;
using namespace eacp::Graphics;

namespace
{
std::vector<unsigned char> readFile(const wchar_t* path)
{
    auto* file = _wfopen(path, L"rb");

    if (file == nullptr)
        return {};

    std::fseek(file, 0, SEEK_END);
    const auto size = std::ftell(file);
    std::fseek(file, 0, SEEK_SET);

    auto bytes = std::vector<unsigned char>((std::size_t) std::max(0L, size));
    const auto read = std::fread(bytes.data(), 1, bytes.size(), file);
    std::fclose(file);

    bytes.resize(read);

    return bytes;
}
} // namespace

auto tPostScriptNameResolves =
    test("FontResolution/postScriptNameResolvesToItsFamily") = []
{
    check(resolveFontFamilyName(L"Arial") == L"Arial");
    check(resolveFontFamilyName(L"ArialMT") == L"Arial");
    check(resolveFontFamilyName(L"NoSuchFace-Regular").empty());
};

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

// Graphics::Font used to hand DirectWrite the raw name against the system
// collection, so an embedded or PostScript name became the fallback family.
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

// DWRITE_TEXT_METRICS.width stops at the last ink, so a lone space measured
// zero; CTLine's typographic width counts trailing whitespace and Windows must
// agree.
auto tSpaceMeasuresItsAdvance = test("TextMetrics/aLoneSpaceMeasuresItsAdvance") = []
{
    const auto font = Font {FontOptions().withName("Arial").withSize(12.f)};

    if (font.getHandle() == nullptr)
        return;

    check(TextMetrics::measureWidth(" ", font) > 0.f);
    check(TextMetrics::measureWidth("a b", font)
          > TextMetrics::measureWidth("ab", font));
};

auto tMemoryFontRegistrationKeepsTheCollectionWhole =
    test("FontResolution/memoryFontRegistrationKeepsSystemFonts") = []
{
    wchar_t windows[MAX_PATH] = {};
    GetWindowsDirectoryW(windows, MAX_PATH);

    const auto bytes =
        readFile((std::wstring {windows} + L"\\Fonts\\arial.ttf").c_str());

    if (bytes.empty())
        return;

    check(registerMemoryFontData(bytes.data(), bytes.size()));

    check(resolveFontFamilyName(L"ArialMT") == L"Arial");
    check(!resolveFontFamilyName(L"Segoe UI").empty());
    check(Text::GlyphRasterizer {Text::FontRequest {.family = "Arial"}}.isValid());
};
