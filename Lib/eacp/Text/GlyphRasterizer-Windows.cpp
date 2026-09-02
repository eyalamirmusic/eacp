#include "GlyphRasterizer.h"
#include "Utf8.h"

#include <eacp/Core/Utils/WinInclude.h>
#include <eacp/Graphics/D2D-Windows.h>
#include <eacp/Core/Utils/Strings.h>

// d2d1.h first: DWRITE_COLOR_F is an alias for the D2D colour struct, and
// dwrite_3.h only picks up the C++ spelling when D2D's headers came before it.
#include <d2d1.h>
#include <dwrite_3.h>
#include <wrl/client.h>

#include <algorithm>
#include <cstdint>
#include <map>
#include <string>
#include <vector>

// DirectWrite rasterizer and shaper, the Windows counterpart to
// GlyphRasterizer-Apple.mm.
//
// Shaping goes through IDWriteTextLayout with a renderer that collects the
// glyph runs instead of drawing them: the layout kerns, ligates, places marks
// and falls back to other faces for what the family lacks, and DrawGlyphRun
// hands back each run's face, glyph ids, advances, offsets and cluster map.
//
// Rasterizing goes through IDWriteGlyphRunAnalysis rather than through Direct2D:
// the analysis hands back a coverage texture straight out of DirectWrite, which
// is exactly what the atlas stores, and it needs no device, no render target and
// no window. That is what lets this run in a headless test as happily as in a
// frame.
//
// The texture type names bytes per pixel, not whether antialiasing happened —
// which reads exactly backwards. DWRITE_TEXTURE_ALIASED_1x1 is one byte per
// pixel and is what grayscale antialiasing fills, at full coverage resolution;
// DWRITE_TEXTURE_CLEARTYPE_3x1 is the three-byte subpixel layout and reports
// *empty bounds* under grayscale. Aliasing is chosen by the rendering mode
// (DWRITE_RENDERING_MODE_ALIASED), never by the texture type.

namespace eacp::Text
{
namespace
{
using Microsoft::WRL::ComPtr;

// Grayscale, never ClearType. The atlas stores coverage and the colour arrives
// at draw time, so subpixel antialiasing would bake one particular text colour
// into every cached glyph. The matching texture is the one-byte-per-pixel one,
// which is already the mask layout the atlas wants.
constexpr auto antialiasMode = DWRITE_TEXT_ANTIALIAS_MODE_GRAYSCALE;
constexpr auto textureType = DWRITE_TEXTURE_ALIASED_1x1;

// Shared, so this is the same underlying factory the Graphics module creates —
// eacp-text deliberately does not reach into eacp-graphics for it, and with
// DWRITE_FACTORY_TYPE_SHARED it does not need to.
IDWriteFactory2* dwriteFactory()
{
    static auto instance = []
    {
        auto created = ComPtr<IDWriteFactory2>();

        DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED,
                            __uuidof(IDWriteFactory2),
                            reinterpret_cast<IUnknown**>(created.GetAddressOf()));

        return created;
    }();

    return instance.Get();
}

// DirectWrite's weights are CSS's numbers, so the class is the enum.
DWRITE_FONT_WEIGHT weightFor(const FontVariant& variant)
{
    return static_cast<DWRITE_FONT_WEIGHT>(weightClass(variant.weight));
}

DWRITE_FONT_STYLE slantFor(const FontVariant& variant)
{
    return variant.italic ? DWRITE_FONT_STYLE_ITALIC : DWRITE_FONT_STYLE_NORMAL;
}

// What CSS's oblique is on the slnt axis, which counts degrees anticlockwise
// from upright and so runs negative for the way a Latin italic leans.
constexpr float obliqueSlant = -14.f;

// The axis values a variant asks of a face, and the simulations they make
// unnecessary. Only ever what matching a face could not supply: the weight
// when the family had no face at it, and the slant when it had no italic one,
// so a family cut into faces and a face matched exactly ask nothing.
//
// A variable face is the one case matching cannot answer. The collection files
// one file under one family, so GetFirstMatchingFont has no bolder sibling to
// return and answers every weight with the same face; the weight has to be
// asked of the axis instead. The CoreText side does the same — see
// GlyphRasterizer-Apple.mm's axisSettingsFor.
struct AxisRequest
{
    std::vector<DWRITE_FONT_AXIS_VALUE> values;
    int supplied = DWRITE_FONT_SIMULATIONS_NONE;
};

AxisRequest axisRequestFor(IDWriteFontResource* resource,
                           IDWriteFont* matched,
                           const FontVariant& variant)
{
    auto request = AxisRequest {};

    if (resource == nullptr || matched == nullptr)
        return request;

    const auto count = resource->GetFontAxisCount();

    if (count == 0)
        return request;

    auto defaults = std::vector<DWRITE_FONT_AXIS_VALUE>(count);
    auto ranges = std::vector<DWRITE_FONT_AXIS_RANGE>(count);

    if (FAILED(resource->GetDefaultFontAxisValues(defaults.data(), count))
        || FAILED(resource->GetFontAxisRanges(ranges.data(), count)))
        return request;

    const auto wanted = weightClass(variant.weight);
    const auto wantsWeight =
        wanted != weightClass(static_cast<int>(matched->GetWeight()));
    const auto wantsSlant =
        variant.italic && matched->GetStyle() == DWRITE_FONT_STYLE_NORMAL;

    auto hasItalicAxis = false;

    for (auto index = UINT32 {}; index < count; ++index)
        if (defaults[index].axisTag == DWRITE_FONT_AXIS_TAG_ITALIC)
            hasItalicAxis = true;

    for (auto index = UINT32 {}; index < count; ++index)
    {
        const auto tag = defaults[index].axisTag;
        const auto& range = ranges[index];
        const auto clamped = [&range](float value)
        { return std::clamp(value, range.minValue, range.maxValue); };

        if (tag == DWRITE_FONT_AXIS_TAG_WEIGHT && wantsWeight)
        {
            request.values.push_back({tag, clamped(static_cast<float>(wanted))});
            request.supplied |= DWRITE_FONT_SIMULATIONS_BOLD;
        }
        else if (tag == DWRITE_FONT_AXIS_TAG_ITALIC && wantsSlant)
        {
            request.values.push_back({tag, clamped(1.f)});
            request.supplied |= DWRITE_FONT_SIMULATIONS_OBLIQUE;
        }
        else if (tag == DWRITE_FONT_AXIS_TAG_SLANT && wantsSlant && !hasItalicAxis)
        {
            request.values.push_back({tag, clamped(obliqueSlant)});
            request.supplied |= DWRITE_FONT_SIMULATIONS_OBLIQUE;
        }
    }

    return request;
}

// The axis values a face was built at. A layout names a family and a weight,
// which finds a face and not a point on an axis, so a variable face has to be
// shaped at the same values the glyphs are rasterized from.
std::vector<DWRITE_FONT_AXIS_VALUE> axisValuesOf(IDWriteFontFace* face)
{
    auto values = std::vector<DWRITE_FONT_AXIS_VALUE> {};
    auto varying = ComPtr<IDWriteFontFace5>();

    if (face == nullptr
        || FAILED(face->QueryInterface(IID_PPV_ARGS(varying.GetAddressOf())))
        || !varying->HasVariations())
        return values;

    values.resize(varying->GetFontAxisValueCount());

    if (FAILED(varying->GetFontAxisValues(values.data(),
                                          static_cast<UINT32>(values.size()))))
        values.clear();

    return values;
}

// UTF-8 to UTF-16, remembering which byte each code unit came from, so a
// glyph's cluster maps back to an offset in the text the caller gave.
struct Utf16Text
{
    std::wstring units;
    std::vector<int> byteOf;
};

Utf16Text toUtf16(std::string_view text)
{
    auto result = Utf16Text {};
    result.units.reserve(text.size());
    result.byteOf.reserve(text.size() + 1);

    auto index = std::size_t {0};

    while (index < text.size())
    {
        const auto start = index;
        const auto codepoint = decodeUtf8(text, index);

        if (codepoint <= 0xFFFF)
        {
            result.units.push_back(static_cast<wchar_t>(codepoint));
            result.byteOf.push_back(static_cast<int>(start));
        }
        else
        {
            const auto value = codepoint - 0x10000;
            result.units.push_back(static_cast<wchar_t>(0xD800 + (value >> 10)));
            result.units.push_back(static_cast<wchar_t>(0xDC00 + (value & 0x3FF)));
            result.byteOf.push_back(static_cast<int>(start));
            result.byteOf.push_back(static_cast<int>(start));
        }
    }

    result.byteOf.push_back(static_cast<int>(text.size()));

    return result;
}

// One glyph run the layout would have drawn, kept instead: the face and
// size it shaped in, and the glyphs with where they went.
struct CollectedRun
{
    ComPtr<IDWriteFontFace> face;
    float emSize = 0.f;
    float baselineX = 0.f;
    std::vector<UINT16> glyphs;
    std::vector<float> advances;
    std::vector<DWRITE_GLYPH_OFFSET> offsets;

    // The first text position (in UTF-16 units of the whole string) each
    // glyph came from, inverted from the run's cluster map.
    std::vector<UINT32> firstPosition;
};

// Whether two faces are the same font: the same files at the same index with
// the same simulations. The layout hands out face objects of its own, so a
// pointer comparison would number the requested face as a fallback.
bool sameFontFace(IDWriteFontFace* a, IDWriteFontFace* b)
{
    if (a == b)
        return true;

    if (a == nullptr || b == nullptr)
        return false;

    if (a->GetIndex() != b->GetIndex() || a->GetSimulations() != b->GetSimulations())
        return false;

    const auto filesOf = [](IDWriteFontFace* face)
    {
        auto keys = std::vector<std::string> {};
        auto count = UINT32 {};

        if (FAILED(face->GetFiles(&count, nullptr)) || count == 0)
            return keys;

        auto raw = std::vector<IDWriteFontFile*>(count);

        if (FAILED(face->GetFiles(&count, raw.data())))
            return keys;

        for (auto index = UINT32 {}; index < count; ++index)
        {
            auto file = ComPtr<IDWriteFontFile> {};
            file.Attach(raw[index]);

            const void* key = nullptr;
            auto size = UINT32 {};

            if (SUCCEEDED(file->GetReferenceKey(&key, &size)) && key != nullptr)
                keys.emplace_back(static_cast<const char*>(key), size);
            else
                keys.emplace_back();
        }

        return keys;
    };

    return filesOf(a) == filesOf(b);
}

// The renderer IDWriteTextLayout::Draw calls back into. It lives on the
// stack for one Draw and never outlives it, so the reference counting is
// deliberately inert.
class RunCollector final : public IDWriteTextRenderer
{
public:
    std::vector<CollectedRun> runs;

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID id,
                                             void** object) noexcept override
    {
        if (id == __uuidof(IUnknown) || id == __uuidof(IDWritePixelSnapping)
            || id == __uuidof(IDWriteTextRenderer))
        {
            *object = this;
            return S_OK;
        }

        *object = nullptr;
        return E_NOINTERFACE;
    }

    ULONG STDMETHODCALLTYPE AddRef() noexcept override { return 1; }
    ULONG STDMETHODCALLTYPE Release() noexcept override { return 1; }

    // Positions are wanted as the shaper placed them, unsnapped: the atlas lays
    // text out in a continuous space, as CoreText does.
    HRESULT STDMETHODCALLTYPE
        IsPixelSnappingDisabled(void*, BOOL* isDisabled) noexcept override
    {
        *isDisabled = TRUE;
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE
        GetCurrentTransform(void*, DWRITE_MATRIX* transform) noexcept override
    {
        *transform = DWRITE_MATRIX {1.f, 0.f, 0.f, 1.f, 0.f, 0.f};
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE GetPixelsPerDip(void*,
                                              FLOAT* pixelsPerDip) noexcept override
    {
        *pixelsPerDip = 1.f;
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE
        DrawGlyphRun(void*,
                     FLOAT baselineOriginX,
                     FLOAT,
                     DWRITE_MEASURING_MODE,
                     const DWRITE_GLYPH_RUN* glyphRun,
                     const DWRITE_GLYPH_RUN_DESCRIPTION* description,
                     IUnknown*) noexcept override
    {
        if (glyphRun == nullptr || glyphRun->glyphCount == 0)
            return S_OK;

        auto run = CollectedRun {};
        run.face = glyphRun->fontFace;
        run.emSize = glyphRun->fontEmSize;
        run.baselineX = baselineOriginX;
        run.glyphs.assign(glyphRun->glyphIndices,
                          glyphRun->glyphIndices + glyphRun->glyphCount);
        run.advances.assign(glyphRun->glyphAdvances,
                            glyphRun->glyphAdvances + glyphRun->glyphCount);

        if (glyphRun->glyphOffsets != nullptr)
            run.offsets.assign(glyphRun->glyphOffsets,
                               glyphRun->glyphOffsets + glyphRun->glyphCount);
        else
            run.offsets.assign(glyphRun->glyphCount, DWRITE_GLYPH_OFFSET {});

        run.firstPosition.assign(glyphRun->glyphCount, UINT32_MAX);

        if (description != nullptr && description->clusterMap != nullptr)
            for (auto position = UINT32 {}; position < description->stringLength;
                 ++position)
            {
                const auto glyph = description->clusterMap[position];

                if (glyph < glyphRun->glyphCount)
                    run.firstPosition[glyph] =
                        std::min(run.firstPosition[glyph],
                                 description->textPosition + position);
            }

        // A glyph the cluster map never names - a mark folded into its base's
        // cluster - takes the position of the glyph before it.
        auto last = description != nullptr ? description->textPosition : UINT32 {};

        for (auto& position: run.firstPosition)
        {
            if (position == UINT32_MAX)
                position = last;

            last = position;
        }

        runs.push_back(std::move(run));

        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE DrawUnderline(
        void*, FLOAT, FLOAT, const DWRITE_UNDERLINE*, IUnknown*) noexcept override
    {
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE DrawStrikethrough(void*,
                                                FLOAT,
                                                FLOAT,
                                                const DWRITE_STRIKETHROUGH*,
                                                IUnknown*) noexcept override
    {
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE DrawInlineObject(void*,
                                               FLOAT,
                                               FLOAT,
                                               IDWriteInlineObject*,
                                               BOOL,
                                               BOOL,
                                               IUnknown*) noexcept override
    {
        return S_OK;
    }
};

// One layer of a colour glyph, already measured. COLR fonts describe an emoji
// as a stack of monochrome glyphs each painted in its own palette colour.
struct ColorLayer
{
    ComPtr<IDWriteGlyphRunAnalysis> analysis;
    RECT bounds {};
    DWRITE_COLOR_F color {};
};

std::uint8_t toByte(float value)
{
    return static_cast<std::uint8_t>(std::clamp(value * 255.f + 0.5f, 0.f, 255.f));
}
} // namespace

struct GlyphRasterizer::Native
{
    explicit Native(const FontRequest& requestToUse)
        : request(requestToUse)
    {
        auto* factory = dwriteFactory();

        if (factory == nullptr)
            return;

        // The collection shared with eacp-graphics (FontRegistry-Windows.cpp):
        // system fonts plus everything registerMemoryFont added, so the
        // rasterizer and the Graphics text sites resolve the same faces.
        collection = Graphics::getFontCollection();

        if (!collection)
            return;

        family = resolveFamily();

        if (family.empty())
            return;

        valid = faceFor({}) != nullptr;
    }

    // Nothing on Windows is called Menlo or SF Mono. Rather than refuse to draw,
    // substitute the way CoreText does for an unknown name — a face that exists
    // beats no text at all, and callers wanting a specific one ask for a family
    // the platform ships.
    std::wstring resolveFamily() const
    {
        // The shared resolver accepts family, PostScript and full names — the
        // CoreText matching rules, so the one name callers ship works on both
        // platforms.
        if (auto resolved =
                Graphics::resolveFontFamilyName(Strings::widen(request.family));
            !resolved.empty())
            return resolved;

        for (const auto* candidate: {L"Segoe UI", L"Arial", L"Consolas"})
            if (hasFamily(candidate))
                return candidate;

        return {};
    }

    bool hasFamily(const std::wstring& name) const
    {
        auto index = UINT32 {};
        auto exists = BOOL {};

        return SUCCEEDED(collection->FindFamilyName(name.c_str(), &index, &exists))
               && exists;
    }

    ComPtr<IDWriteFontFace> createFace(const FontVariant& variant) const
    {
        auto index = UINT32 {};
        auto exists = BOOL {};

        if (FAILED(collection->FindFamilyName(family.c_str(), &index, &exists))
            || !exists)
            return {};

        auto fontFamily = ComPtr<IDWriteFontFamily>();

        if (FAILED(collection->GetFontFamily(index, fontFamily.GetAddressOf())))
            return {};

        auto font = ComPtr<IDWriteFont>();

        if (FAILED(fontFamily->GetFirstMatchingFont(weightFor(variant),
                                                    DWRITE_FONT_STRETCH_NORMAL,
                                                    slantFor(variant),
                                                    font.GetAddressOf())))
            return {};

        auto face = ComPtr<IDWriteFontFace>();

        if (FAILED(font->CreateFontFace(face.GetAddressOf())))
            return {};

        const auto simulations = missingTraits(font.Get(), variant);

        if (auto varied = alongItsOwnAxes(face, font.Get(), variant, simulations))
            return varied;

        return withSimulations(face.Get(), simulations);
    }

    // The face moved along its own variation axes to the weight and slant the
    // family had no face for, with only the simulations the axes did not
    // supply. Nothing when the face does not vary, when nothing has to be
    // asked of it, or when the interfaces are not there — the caller then
    // falls back on the simulations alone, which is what it did before.
    static ComPtr<IDWriteFontFace>
        alongItsOwnAxes(const ComPtr<IDWriteFontFace>& face,
                        IDWriteFont* matched,
                        const FontVariant& variant,
                        DWRITE_FONT_SIMULATIONS simulations)
    {
        auto varying = ComPtr<IDWriteFontFace5>();

        if (FAILED(face.As(&varying)) || !varying->HasVariations())
            return {};

        auto resource = ComPtr<IDWriteFontResource>();

        if (FAILED(varying->GetFontResource(resource.GetAddressOf())))
            return {};

        const auto request = axisRequestFor(resource.Get(), matched, variant);

        if (request.values.empty())
            return {};

        const auto rest =
            static_cast<DWRITE_FONT_SIMULATIONS>(simulations & ~request.supplied);
        const auto count = static_cast<UINT32>(request.values.size());
        auto varied = ComPtr<IDWriteFontFace5>();

        if (FAILED(resource->CreateFontFace(
                rest, request.values.data(), count, varied.GetAddressOf())))
            return {};

        return varied;
    }

    // GetFirstMatchingFont returns the nearest face rather than failing, so a
    // family shipping only Regular answers a bold request with Regular. Ask
    // DirectWrite to synthesize whatever the family does not have, which is what
    // CTFontCreateCopyWithSymbolicTraits does on the Apple side.
    static DWRITE_FONT_SIMULATIONS missingTraits(IDWriteFont* font,
                                                 const FontVariant& variant)
    {
        auto simulations = int {DWRITE_FONT_SIMULATIONS_NONE};

        if (variant.weight >= 600
            && font->GetWeight() < DWRITE_FONT_WEIGHT_SEMI_BOLD)
            simulations |= DWRITE_FONT_SIMULATIONS_BOLD;

        if (variant.italic && font->GetStyle() == DWRITE_FONT_STYLE_NORMAL)
            simulations |= DWRITE_FONT_SIMULATIONS_OBLIQUE;

        return static_cast<DWRITE_FONT_SIMULATIONS>(simulations);
    }

    static ComPtr<IDWriteFontFace>
        withSimulations(IDWriteFontFace* face, DWRITE_FONT_SIMULATIONS simulations)
    {
        if (simulations == DWRITE_FONT_SIMULATIONS_NONE)
            return face;

        auto fileCount = UINT32 {};

        if (FAILED(face->GetFiles(&fileCount, nullptr)) || fileCount == 0)
            return face;

        auto raw = std::vector<IDWriteFontFile*>(fileCount);

        if (FAILED(face->GetFiles(&fileCount, raw.data())))
            return face;

        // GetFiles hands back references the caller owns; adopt them so they are
        // released however this returns.
        auto owned = std::vector<ComPtr<IDWriteFontFile>>(fileCount);

        for (auto index = UINT32 {}; index < fileCount; ++index)
            owned[index].Attach(raw[index]);

        auto derived = ComPtr<IDWriteFontFace>();

        dwriteFactory()->CreateFontFace(face->GetType(),
                                        fileCount,
                                        raw.data(),
                                        face->GetIndex(),
                                        simulations,
                                        derived.GetAddressOf());

        return derived ? derived : ComPtr<IDWriteFontFace>(face);
    }

    // The face for a variant, built on first ask: the family's nearest by
    // DirectWrite's matching, with what it lacks simulated.
    IDWriteFontFace* faceFor(const FontVariant& variant) const
    {
        const auto key = weightClass(variant.weight) * 2 + (variant.italic ? 1 : 0);
        auto found = variants.find(key);

        if (found != variants.end())
            return found->second.Get();

        auto face = createFace(variant);
        auto* result = face.Get();
        variants.emplace(key, std::move(face));

        return result;
    }

    FontMetrics metrics(const FontVariant& variant) const
    {
        auto result = FontMetrics {};
        auto* face = faceFor(variant);

        if (face == nullptr)
            return result;

        auto fontMetrics = DWRITE_FONT_METRICS {};
        face->GetMetrics(&fontMetrics);

        const auto perUnit =
            request.pixelSize() / static_cast<float>(fontMetrics.designUnitsPerEm);

        result.ascent = fontMetrics.ascent * perUnit;
        result.descent = fontMetrics.descent * perUnit;

        // lineGap is signed in DirectWrite and a few faces report it negative.
        // CoreText never returns negative leading and the atlas adds it straight
        // into line height, so clamp rather than let lines overlap.
        result.leading = std::max(0.f, fontMetrics.lineGap * perUnit);

        // 'M' is the conventional width probe; on a monospace face every glyph
        // shares this advance, and on a proportional one it is only a hint.
        const auto reference = UINT32 {'M'};
        auto glyph = UINT16 {};

        if (SUCCEEDED(face->GetGlyphIndices(&reference, 1, &glyph)) && glyph != 0)
            result.advance = advanceOf(face, glyph, request.pixelSize());

        return result;
    }

    static float advanceOf(IDWriteFontFace* face, UINT16 glyph, float emSize)
    {
        auto fontMetrics = DWRITE_FONT_METRICS {};
        face->GetMetrics(&fontMetrics);

        auto glyphMetrics = DWRITE_GLYPH_METRICS {};

        if (FAILED(face->GetDesignGlyphMetrics(&glyph, 1, &glyphMetrics, FALSE)))
            return 0.f;

        // Design metrics, not the hinted ones the analysis would produce: the
        // atlas lays text out in a continuous space, and CoreText reports
        // unhinted advances too, so this keeps the two platforms in step.
        return glyphMetrics.advanceWidth * emSize
               / static_cast<float>(fontMetrics.designUnitsPerEm);
    }

    // A face the layout reached for that is not the one asked for, at the
    // size the layout drew it: the fallback's em can differ from the base's.
    struct FallbackFace
    {
        ComPtr<IDWriteFontFace> face;
        float emSize = 0.f;
    };

    // The number a run's face shapes under: 0 for the face asked for, else
    // the fallback's place in the table, added the first time it is met.
    int fontIndexOf(IDWriteFontFace* face,
                    float emSize,
                    IDWriteFontFace* requested) const
    {
        if (face == nullptr || sameFontFace(face, requested))
            return 0;

        for (auto index = std::size_t {0}; index < fallbacks.size(); ++index)
            if (sameFontFace(fallbacks[index].face.Get(), face))
                return static_cast<int>(index) + 1;

        if (fallbacks.size() >= 255)
            return 0;

        fallbacks.push_back({face, emSize});

        return static_cast<int>(fallbacks.size());
    }

    struct FaceAt
    {
        IDWriteFontFace* face = nullptr;
        float emSize = 0.f;
    };

    FaceAt faceOf(GlyphKey key, const FontVariant& variant) const
    {
        if (key.font == 0)
            return {faceFor(variant), request.pixelSize()};

        const auto index = static_cast<std::size_t>(key.font) - 1;

        if (index >= fallbacks.size())
            return {};

        return {fallbacks[index].face.Get(), fallbacks[index].emSize};
    }

    ShapedRun shape(std::string_view text, const FontVariant& variant) const
    {
        auto result = ShapedRun {};
        auto* factory = dwriteFactory();
        auto* requested = faceFor(variant);

        if (factory == nullptr || requested == nullptr || text.empty())
            return result;

        auto format = ComPtr<IDWriteTextFormat>();

        if (FAILED(factory->CreateTextFormat(family.c_str(),
                                             collection.Get(),
                                             weightFor(variant),
                                             slantFor(variant),
                                             DWRITE_FONT_STRETCH_NORMAL,
                                             request.pixelSize(),
                                             L"en-us",
                                             format.GetAddressOf())))
            return result;

        format->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);

        const auto utf16 = toUtf16(text);
        auto layout = ComPtr<IDWriteTextLayout>();

        if (FAILED(factory->CreateTextLayout(utf16.units.c_str(),
                                             static_cast<UINT32>(utf16.units.size()),
                                             format.Get(),
                                             1.0e6f,
                                             1.0e6f,
                                             layout.GetAddressOf())))
            return result;

        if (const auto axes = axisValuesOf(requested); !axes.empty())
            if (auto varying = ComPtr<IDWriteTextLayout4>();
                SUCCEEDED(layout.As(&varying)))
                varying->SetFontAxisValues(
                    axes.data(),
                    static_cast<UINT32>(axes.size()),
                    {0, static_cast<UINT32>(utf16.units.size())});

        auto collector = RunCollector {};

        if (FAILED(layout->Draw(nullptr, &collector, 0.f, 0.f)))
            return result;

        for (const auto& run: collector.runs)
        {
            const auto fontIndex =
                fontIndexOf(run.face.Get(), run.emSize, requested);
            auto pen = run.baselineX;

            for (auto index = std::size_t {0}; index < run.glyphs.size(); ++index)
            {
                const auto unit =
                    std::min(static_cast<std::size_t>(run.firstPosition[index]),
                             utf16.byteOf.size() - 1);

                result.glyphs.add({{run.glyphs[index], fontIndex},
                                   pen + run.offsets[index].advanceOffset,
                                   run.offsets[index].ascenderOffset,
                                   utf16.byteOf[unit]});

                pen += run.advances[index];
            }
        }

        auto metrics = DWRITE_TEXT_METRICS {};

        if (SUCCEEDED(layout->GetMetrics(&metrics)))
            result.advance = metrics.widthIncludingTrailingWhitespace;

        return result;
    }

    GlyphBitmap rasterize(GlyphKey key,
                          const FontVariant& variant,
                          const RasterRequest& request) const
    {
        auto result = GlyphBitmap {};
        const auto at = faceOf(key, variant);

        if (at.face == nullptr)
            return result;

        auto glyph = static_cast<UINT16>(key.glyph);

        result.valid = true;
        result.advance = advanceOf(at.face, glyph, at.emSize);

        auto advance = FLOAT {0.f};
        auto offset = DWRITE_GLYPH_OFFSET {};
        auto run = DWRITE_GLYPH_RUN {};

        run.fontFace = at.face;
        run.fontEmSize = at.emSize;
        run.glyphCount = 1;
        run.glyphIndices = &glyph;
        run.glyphAdvances = &advance;
        run.glyphOffsets = &offset;

        // DirectWrite's coverage does not follow the text's lightness the way
        // CoreGraphics's does, so the request's lightText has nothing to say
        // here.
        if (!drawColorGlyph(run, result))
            drawMask(run, result, request.subpixelX);

        return result;
    }

    ComPtr<IDWriteGlyphRunAnalysis>
        analyse(const DWRITE_GLYPH_RUN& run, float baselineX, float baselineY) const
    {
        auto analysis = ComPtr<IDWriteGlyphRunAnalysis>();
        auto* factory = dwriteFactory();

        if (factory == nullptr)
            return analysis;

        factory->CreateGlyphRunAnalysis(&run,
                                        nullptr,
                                        DWRITE_RENDERING_MODE_NATURAL_SYMMETRIC,
                                        DWRITE_MEASURING_MODE_NATURAL,
                                        DWRITE_GRID_FIT_MODE_DEFAULT,
                                        antialiasMode,
                                        baselineX,
                                        baselineY,
                                        analysis.GetAddressOf());

        return analysis;
    }

    static bool measure(IDWriteGlyphRunAnalysis* analysis, RECT& bounds)
    {
        if (FAILED(analysis->GetAlphaTextureBounds(textureType, &bounds)))
            return false;

        return bounds.right > bounds.left && bounds.bottom > bounds.top;
    }

    void drawMask(const DWRITE_GLYPH_RUN& run,
                  GlyphBitmap& bitmap,
                  float subpixelX) const
    {
        // The baseline origin carries the subpixel offset: the texture bounds
        // come back relative to the whole pixel, which is the bearing the atlas
        // wants, and DirectWrite draws the outline the fraction to the right.
        auto analysis = analyse(run, std::clamp(subpixelX, 0.f, 1.f), 0.f);
        auto bounds = RECT {};

        // An empty box is a space: valid, advances the pen, draws nothing.
        if (!analysis || !measure(analysis.Get(), bounds))
            return;

        takeBounds(bounds, bitmap);

        // One byte of coverage per pixel is already the mask format, so the
        // texture is filled straight into the bitmap with nothing in between.
        bitmap.pixels.resize(static_cast<std::size_t>(bitmap.width) * bitmap.height);

        if (FAILED(analysis->CreateAlphaTexture(
                textureType,
                &bounds,
                bitmap.pixels.data(),
                static_cast<UINT32>(bitmap.pixels.size()))))
        {
            bitmap.width = 0;
            bitmap.height = 0;
            bitmap.pixels.clear();
        }
    }

    static void takeBounds(const RECT& bounds, GlyphBitmap& bitmap)
    {
        bitmap.width = bounds.right - bounds.left;
        bitmap.height = bounds.bottom - bounds.top;
        bitmap.bearingX = static_cast<float>(bounds.left);

        // DirectWrite measures the texture box downwards from the baseline;
        // GlyphBitmap measures its top edge upwards from it.
        bitmap.bearingY = static_cast<float>(-bounds.top);
    }

    // Returns false for the overwhelmingly common case of a glyph that is not
    // from a COLR font, which then takes the mask path.
    bool drawColorGlyph(const DWRITE_GLYPH_RUN& run, GlyphBitmap& bitmap) const
    {
        auto* factory = dwriteFactory();
        auto layers = ComPtr<IDWriteColorGlyphRunEnumerator>();

        if (factory == nullptr)
            return false;

        if (FAILED(factory->TranslateColorGlyphRun(0.f,
                                                   0.f,
                                                   &run,
                                                   nullptr,
                                                   DWRITE_MEASURING_MODE_NATURAL,
                                                   nullptr,
                                                   0,
                                                   layers.GetAddressOf()))
            || !layers)
            return false;

        bitmap.format = GlyphFormat::Color;

        const auto collected = collectLayers(layers.Get());

        // A colour font's blank glyph is still a colour glyph.
        if (!collected.empty())
            compositeLayers(collected, bitmap);

        return true;
    }

    std::vector<ColorLayer>
        collectLayers(IDWriteColorGlyphRunEnumerator* layers) const
    {
        auto collected = std::vector<ColorLayer> {};
        auto hasMore = BOOL {};

        while (SUCCEEDED(layers->MoveNext(&hasMore)) && hasMore)
        {
            const DWRITE_COLOR_GLYPH_RUN* layer = nullptr;

            if (FAILED(layers->GetCurrentRun(&layer)) || layer == nullptr)
                continue;

            auto entry = ColorLayer {};

            entry.analysis = analyse(
                layer->glyphRun, layer->baselineOriginX, layer->baselineOriginY);

            // A palette index of 0xffff means "paint this layer in the text
            // colour". Colour glyphs are drawn untinted, so white is the only
            // colour that leaves the result unchanged.
            entry.color = layer->paletteIndex == 0xffff
                              ? DWRITE_COLOR_F {1.f, 1.f, 1.f, 1.f}
                              : layer->runColor;

            if (entry.analysis && measure(entry.analysis.Get(), entry.bounds))
                collected.push_back(std::move(entry));
        }

        return collected;
    }

    static void compositeLayers(const std::vector<ColorLayer>& layers,
                                GlyphBitmap& bitmap)
    {
        auto bounds = layers.front().bounds;

        for (const auto& layer: layers)
        {
            bounds.left = std::min(bounds.left, layer.bounds.left);
            bounds.top = std::min(bounds.top, layer.bounds.top);
            bounds.right = std::max(bounds.right, layer.bounds.right);
            bounds.bottom = std::max(bounds.bottom, layer.bounds.bottom);
        }

        takeBounds(bounds, bitmap);

        const auto pixelCount =
            static_cast<std::size_t>(bitmap.width) * bitmap.height;

        // Composited premultiplied, where 'over' is a plain lerp, then converted
        // to the straight alpha the atlas stores. Layers arrive bottom first.
        auto accumulated = std::vector<float>(pixelCount * 4, 0.f);

        for (const auto& layer: layers)
            blendLayer(layer, bounds, bitmap.width, accumulated);

        bitmap.pixels.resize(pixelCount * 4);

        for (auto index = std::size_t {}; index < pixelCount; ++index)
        {
            const auto alpha = accumulated[index * 4 + 3];

            bitmap.pixels[index * 4 + 3] = toByte(alpha);

            for (auto channel = 0; channel < 3; ++channel)
                bitmap.pixels[index * 4 + channel] =
                    alpha > 0.f ? toByte(accumulated[index * 4 + channel] / alpha)
                                : std::uint8_t {};
        }
    }

    static void blendLayer(const ColorLayer& layer,
                           const RECT& bounds,
                           int width,
                           std::vector<float>& target)
    {
        const auto layerWidth = layer.bounds.right - layer.bounds.left;
        const auto layerHeight = layer.bounds.bottom - layer.bounds.top;
        const auto span = static_cast<std::size_t>(layerWidth) * layerHeight;

        auto texture = std::vector<std::uint8_t>(span);

        if (FAILED(layer.analysis->CreateAlphaTexture(
                textureType,
                &layer.bounds,
                texture.data(),
                static_cast<UINT32>(texture.size()))))
            return;

        for (auto y = 0; y < layerHeight; ++y)
        {
            for (auto x = 0; x < layerWidth; ++x)
            {
                const auto source = static_cast<std::size_t>(y) * layerWidth + x;
                const auto coverage = texture[source] / 255.f;

                if (coverage <= 0.f)
                    continue;

                const auto alpha = coverage * layer.color.a;
                const auto row = y + layer.bounds.top - bounds.top;
                const auto column = x + layer.bounds.left - bounds.left;
                const auto at = (static_cast<std::size_t>(row) * width + column) * 4;

                const float channels[3] = {
                    layer.color.r, layer.color.g, layer.color.b};

                for (auto channel = 0; channel < 3; ++channel)
                    target[at + channel] = channels[channel] * alpha
                                           + target[at + channel] * (1.f - alpha);

                target[at + 3] = alpha + target[at + 3] * (1.f - alpha);
            }
        }
    }

    FontRequest request;
    ComPtr<IDWriteFontCollection> collection;
    std::wstring family;
    mutable std::map<int, ComPtr<IDWriteFontFace>> variants;
    mutable std::vector<FallbackFace> fallbacks;
    bool valid = false;
};

GlyphRasterizer::GlyphRasterizer(const FontRequest& request)
    : impl(request)
{
}

GlyphRasterizer::~GlyphRasterizer() = default;

bool GlyphRasterizer::isValid() const
{
    return impl->valid;
}

std::string GlyphRasterizer::resolvedFamily() const
{
    return Strings::narrow(impl->family);
}

FontMetrics GlyphRasterizer::metrics(const FontVariant& variant) const
{
    return impl->metrics(variant);
}

float GlyphRasterizer::scale() const
{
    return impl->request.scale;
}

ShapedRun GlyphRasterizer::shape(std::string_view text,
                                 const FontVariant& variant) const
{
    return impl->shape(text, variant);
}

GlyphBitmap GlyphRasterizer::rasterize(GlyphKey glyph,
                                       const FontVariant& variant,
                                       const RasterRequest& request) const
{
    return impl->rasterize(glyph, variant, request);
}

GlyphBitmap GlyphRasterizer::rasterize(char32_t codepoint, FontStyle style) const
{
    char encoded[4] = {};
    const auto length = encodeUtf8(codepoint, encoded);
    const auto variant = variantOf(style);
    const auto run = impl->shape({encoded, length}, variant);

    if (run.glyphs.empty())
        return {};

    return impl->rasterize(run.glyphs[0].key, variant, {});
}

const FontRequest& GlyphRasterizer::request() const
{
    return impl->request;
}

std::optional<RegisteredFont> registerMemoryFont(const void* data, std::size_t size)
{
    // Registration lives in eacp-graphics so its text sites (Font,
    // TextMetrics) see the face too — the same process-wide visibility
    // CTFontManagerRegisterGraphicsFont gives the Apple side.
    const auto names = Graphics::registerMemoryFontData(data, size);

    if (!names)
        return std::nullopt;

    return RegisteredFont {Strings::narrow(names->family),
                           Strings::narrow(names->postScriptName)};
}
} // namespace eacp::Text
