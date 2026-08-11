#include "GlyphRasterizer.h"

#include <eacp/Core/Utils/WinInclude.h>
#include <eacp/Graphics/D2D-Windows.h>
#include <eacp/Graphics/Helpers/StringUtils-Windows.h>

// d2d1.h first: DWRITE_COLOR_F is an alias for the D2D colour struct, and
// dwrite_3.h only picks up the C++ spelling when D2D's headers came before it.
#include <d2d1.h>
#include <dwrite_3.h>
#include <wrl/client.h>

#include <algorithm>
#include <string>
#include <vector>

// DirectWrite rasterizer, via IDWriteGlyphRunAnalysis rather than Direct2D: no
// device, target or window needed. DWRITE_TEXTURE_* names bytes per pixel, not
// antialiasing -- CLEARTYPE_3x1 reports empty bounds under grayscale.

namespace eacp::Text
{
namespace
{
using Graphics::toWideString;
using Microsoft::WRL::ComPtr;

// Grayscale, never ClearType: subpixel antialiasing would bake one text colour
// into coverage the atlas tints at draw time.
constexpr auto antialiasMode = DWRITE_TEXT_ANTIALIAS_MODE_GRAYSCALE;
constexpr auto textureType = DWRITE_TEXTURE_ALIASED_1x1;

// DWRITE_FACTORY_TYPE_SHARED, so this is the factory the Graphics module made,
// without eacp-text having to reach into eacp-graphics for it.
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

int encodeUtf16(char32_t codepoint, wchar_t* units)
{
    if (codepoint <= 0xffff)
    {
        units[0] = static_cast<wchar_t>(codepoint);
        return 1;
    }

    const auto value = codepoint - 0x10000;
    units[0] = static_cast<wchar_t>(0xd800 + (value >> 10));
    units[1] = static_cast<wchar_t>(0xdc00 + (value & 0x3ff));

    return 2;
}

DWRITE_FONT_WEIGHT weightFor(FontStyle style)
{
    return isBold(style) ? DWRITE_FONT_WEIGHT_BOLD : DWRITE_FONT_WEIGHT_NORMAL;
}

DWRITE_FONT_STYLE slantFor(FontStyle style)
{
    return isItalic(style) ? DWRITE_FONT_STYLE_ITALIC : DWRITE_FONT_STYLE_NORMAL;
}

// The text MapCharacters analyses: one codepoint on the stack that never
// outlives the call, so the reference counting is deliberately inert.
class SingleGlyphSource final : public IDWriteTextAnalysisSource
{
public:
    SingleGlyphSource(const wchar_t* textToUse, UINT32 lengthToUse)
        : text(textToUse)
        , length(lengthToUse)
    {
    }

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID id,
                                             void** object) noexcept override
    {
        if (id == __uuidof(IUnknown) || id == __uuidof(IDWriteTextAnalysisSource))
        {
            *object = this;
            return S_OK;
        }

        *object = nullptr;
        return E_NOINTERFACE;
    }

    ULONG STDMETHODCALLTYPE AddRef() noexcept override { return 1; }
    ULONG STDMETHODCALLTYPE Release() noexcept override { return 1; }

    HRESULT STDMETHODCALLTYPE GetTextAtPosition(UINT32 position,
                                                const WCHAR** textOut,
                                                UINT32* lengthOut) noexcept override
    {
        const auto inside = position < length;

        *textOut = inside ? text + position : nullptr;
        *lengthOut = inside ? length - position : 0;

        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE GetTextBeforePosition(
        UINT32 position, const WCHAR** textOut, UINT32* lengthOut) noexcept override
    {
        const auto inside = position > 0 && position <= length;

        *textOut = inside ? text : nullptr;
        *lengthOut = inside ? position : 0;

        return S_OK;
    }

    DWRITE_READING_DIRECTION STDMETHODCALLTYPE
        GetParagraphReadingDirection() noexcept override
    {
        return DWRITE_READING_DIRECTION_LEFT_TO_RIGHT;
    }

    HRESULT STDMETHODCALLTYPE GetLocaleName(UINT32 position,
                                            UINT32* lengthOut,
                                            const WCHAR** nameOut) noexcept override
    {
        *lengthOut = length - std::min(position, length);
        *nameOut = nullptr;

        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE GetNumberSubstitution(
        UINT32 position,
        UINT32* lengthOut,
        IDWriteNumberSubstitution** substitution) noexcept override
    {
        *lengthOut = length - std::min(position, length);
        *substitution = nullptr;

        return S_OK;
    }

private:
    const wchar_t* text;
    UINT32 length;
};

// A COLR font describes an emoji as a stack of monochrome glyphs, each painted
// in its own palette colour.
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

        // Shared with eacp-graphics (FontRegistry-Windows.cpp): system fonts
        // plus whatever registerMemoryFont added, so both resolve the same
        // faces.
        collection = Graphics::getFontCollection();

        if (!collection)
            return;

        factory->GetSystemFontFallback(fallback.GetAddressOf());

        family = resolveFamily();

        if (family.empty())
            return;

        for (auto index = 0; index < 4; ++index)
            faces[index] = createFace(static_cast<FontStyle>(index));

        valid = faces[0] != nullptr;
    }

    // Nothing on Windows is called Menlo or SF Mono, so an unknown name
    // substitutes the way CoreText does rather than refusing to draw. The
    // shared resolver accepts family, PostScript and full names, as CoreText.
    std::wstring resolveFamily() const
    {
        if (auto resolved =
                Graphics::resolveFontFamilyName(toWideString(request.family));
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

    ComPtr<IDWriteFontFace> createFace(FontStyle style) const
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

        if (FAILED(fontFamily->GetFirstMatchingFont(weightFor(style),
                                                    DWRITE_FONT_STRETCH_NORMAL,
                                                    slantFor(style),
                                                    font.GetAddressOf())))
            return {};

        auto face = ComPtr<IDWriteFontFace>();

        if (FAILED(font->CreateFontFace(face.GetAddressOf())))
            return {};

        return withSimulations(face.Get(), missingTraits(font.Get(), style));
    }

    // GetFirstMatchingFont returns the nearest face rather than failing, so a
    // family shipping only Regular answers a bold request with Regular:
    // DirectWrite synthesizes whatever the family does not have.
    static DWRITE_FONT_SIMULATIONS missingTraits(IDWriteFont* font, FontStyle style)
    {
        auto simulations = int {DWRITE_FONT_SIMULATIONS_NONE};

        if (isBold(style) && font->GetWeight() < DWRITE_FONT_WEIGHT_SEMI_BOLD)
            simulations |= DWRITE_FONT_SIMULATIONS_BOLD;

        if (isItalic(style) && font->GetStyle() == DWRITE_FONT_STYLE_NORMAL)
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

        // GetFiles hands back references the caller owns; adopted so they are
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

    IDWriteFontFace* faceFor(FontStyle style) const
    {
        const auto index = static_cast<int>(style);

        return faces[index] ? faces[index].Get() : faces[0].Get();
    }

    FontMetrics metrics(FontStyle style) const
    {
        auto result = FontMetrics {};
        auto* face = faceFor(style);

        if (face == nullptr)
            return result;

        auto fontMetrics = DWRITE_FONT_METRICS {};
        face->GetMetrics(&fontMetrics);

        const auto perUnit =
            request.pixelSize() / static_cast<float>(fontMetrics.designUnitsPerEm);

        result.ascent = fontMetrics.ascent * perUnit;
        result.descent = fontMetrics.descent * perUnit;

        // lineGap is signed in DirectWrite and a few faces report it negative,
        // where CoreText never does, so it is clamped rather than shortening
        // the line height the atlas adds it into.
        result.leading = std::max(0.f, fontMetrics.lineGap * perUnit);

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
        // atlas lays out in a continuous space, and CoreText is unhinted too.
        return glyphMetrics.advanceWidth * emSize
               / static_cast<float>(fontMetrics.designUnitsPerEm);
    }

    struct Resolved
    {
        ComPtr<IDWriteFontFace> face;
        UINT16 glyph = 0;
        float emSize = 0.f;
    };

    Resolved resolve(char32_t codepoint, FontStyle style) const
    {
        auto result = Resolved {};
        auto* base = faceFor(style);

        if (base == nullptr)
            return result;

        const auto point = static_cast<UINT32>(codepoint);
        auto glyph = UINT16 {};

        if (SUCCEEDED(base->GetGlyphIndices(&point, 1, &glyph)) && glyph != 0)
        {
            result.face = base;
            result.glyph = glyph;
            result.emSize = request.pixelSize();

            return result;
        }

        return mapThroughFallback(codepoint, style);
    }

    // Whichever face the system fallback names, which is how a Latin family
    // still renders CJK and emoji.
    Resolved mapThroughFallback(char32_t codepoint, FontStyle style) const
    {
        auto result = Resolved {};

        if (!fallback)
            return result;

        wchar_t units[2] = {};
        const auto unitCount = static_cast<UINT32>(encodeUtf16(codepoint, units));

        auto source = SingleGlyphSource {units, unitCount};
        auto mappedLength = UINT32 {};
        auto mapped = ComPtr<IDWriteFont>();
        auto scale = FLOAT {1.f};

        fallback->MapCharacters(&source,
                                0,
                                unitCount,
                                collection.Get(),
                                family.c_str(),
                                weightFor(style),
                                slantFor(style),
                                DWRITE_FONT_STRETCH_NORMAL,
                                &mappedLength,
                                mapped.GetAddressOf(),
                                &scale);

        if (!mapped)
            return result;

        auto face = ComPtr<IDWriteFontFace>();

        if (FAILED(mapped->CreateFontFace(face.GetAddressOf())))
            return result;

        const auto point = static_cast<UINT32>(codepoint);
        auto glyph = UINT16 {};

        if (FAILED(face->GetGlyphIndices(&point, 1, &glyph)) || glyph == 0)
            return result;

        result.face = face;
        result.glyph = glyph;

        // A fallback face may need a different em to stay visually matched to
        // the base font; MapCharacters reports the factor.
        result.emSize = request.pixelSize() * scale;

        return result;
    }

    GlyphBitmap rasterize(char32_t codepoint, FontStyle style) const
    {
        auto result = GlyphBitmap {};
        const auto resolved = resolve(codepoint, style);

        if (!resolved.face)
            return result;

        result.valid = true;
        result.advance =
            advanceOf(resolved.face.Get(), resolved.glyph, resolved.emSize);

        auto advance = FLOAT {0.f};
        auto offset = DWRITE_GLYPH_OFFSET {};
        auto run = DWRITE_GLYPH_RUN {};

        run.fontFace = resolved.face.Get();
        run.fontEmSize = resolved.emSize;
        run.glyphCount = 1;
        run.glyphIndices = &resolved.glyph;
        run.glyphAdvances = &advance;
        run.glyphOffsets = &offset;

        if (!drawColorGlyph(run, result))
            drawMask(run, result);

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

    void drawMask(const DWRITE_GLYPH_RUN& run, GlyphBitmap& bitmap) const
    {
        auto analysis = analyse(run, 0.f, 0.f);
        auto bounds = RECT {};

        // An empty box is a space: valid, advances the pen, draws nothing.
        if (!analysis || !measure(analysis.Get(), bounds))
            return;

        takeBounds(bounds, bitmap);

        // One byte of coverage per pixel is already the mask format, so the
        // texture fills the bitmap directly.
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
        // GlyphBitmap measures its top edge upwards from it, hence the sign.
        bitmap.bearingY = static_cast<float>(-bounds.top);
    }

    // False for a glyph that is not from a COLR font, which then takes the mask
    // path.
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

            // Palette index 0xffff means "paint in the text colour", and colour
            // glyphs draw untinted, so only white leaves the result unchanged.
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
    ComPtr<IDWriteFontFallback> fallback;
    ComPtr<IDWriteFontFace> faces[4];
    std::wstring family;
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

FontMetrics GlyphRasterizer::metrics(FontStyle style) const
{
    return impl->metrics(style);
}

float GlyphRasterizer::scale() const
{
    return impl->request.scale;
}

GlyphBitmap GlyphRasterizer::rasterize(char32_t codepoint, FontStyle style) const
{
    return impl->rasterize(codepoint, style);
}

const FontRequest& GlyphRasterizer::request() const
{
    return impl->request;
}

bool registerMemoryFont(const void* data, std::size_t size)
{
    // In eacp-graphics, so its own text sites see the face too.
    return Graphics::registerMemoryFontData(data, size);
}
} // namespace eacp::Text
