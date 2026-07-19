#include "GlyphRasterizer.h"

#include <eacp/Core/Utils/WinInclude.h>
#include <eacp/Graphics/Helpers/StringUtils-Windows.h>

// d2d1.h first: DWRITE_COLOR_F is an alias for the D2D colour struct, and
// dwrite_3.h only picks up the C++ spelling when D2D's headers came before it.
#include <d2d1.h>
#include <dwrite_3.h>
#include <wrl/client.h>

#include <algorithm>
#include <mutex>
#include <string>
#include <vector>

// DirectWrite rasterizer, the Windows counterpart to GlyphRasterizer-Apple.mm.
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
using Graphics::toWideString;
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

// Only registerMemoryFont needs this newer interface, so a machine too old to
// offer it still rasterizes installed fonts.
IDWriteFactory5* memoryFontFactory()
{
    static auto instance = []
    {
        auto created = ComPtr<IDWriteFactory5>();

        if (auto* factory = dwriteFactory())
            factory->QueryInterface(
                __uuidof(IDWriteFactory5),
                reinterpret_cast<void**>(created.GetAddressOf()));

        return created;
    }();

    return instance.Get();
}

// The collection rasterizers resolve family names against: the system one until
// a font is registered from memory, and a rebuilt system-plus-embedded one
// afterwards. DirectWrite has no process-wide registry the way CoreText does,
// so the collection has to be threaded through explicitly.
class FontRegistry
{
public:
    ComPtr<IDWriteFontCollection> collection()
    {
        const auto lock = std::lock_guard {mutex};

        if (!current)
            if (auto* factory = dwriteFactory())
                factory->GetSystemFontCollection(current.GetAddressOf(), FALSE);

        return current;
    }

    bool add(const void* data, std::size_t size)
    {
        auto* factory = memoryFontFactory();

        if (factory == nullptr || data == nullptr || size == 0)
            return false;

        const auto lock = std::lock_guard {mutex};

        if (!loader)
        {
            if (FAILED(factory->CreateInMemoryFontFileLoader(loader.GetAddressOf()))
                || FAILED(factory->RegisterFontFileLoader(loader.Get())))
                return false;
        }

        auto file = ComPtr<IDWriteFontFile>();

        // A null owner tells DirectWrite to copy the data, so the caller's
        // buffer does not have to outlive the registration.
        if (FAILED(loader->CreateInMemoryFontFileReference(factory,
                                                           data,
                                                           static_cast<UINT32>(size),
                                                           nullptr,
                                                           file.GetAddressOf())))
            return false;

        files.push_back(file);

        return rebuild(factory);
    }

private:
    // Called with the lock held. A font set is immutable once built, so adding a
    // face means building a fresh one over every file plus the system set.
    bool rebuild(IDWriteFactory5* factory)
    {
        auto builder = ComPtr<IDWriteFontSetBuilder1>();

        if (FAILED(factory->CreateFontSetBuilder(builder.GetAddressOf())))
            return false;

        for (const auto& file: files)
            builder->AddFontFile(file.Get());

        auto systemSet = ComPtr<IDWriteFontSet>();

        if (SUCCEEDED(factory->GetSystemFontSet(systemSet.GetAddressOf())))
            builder->AddFontSet(systemSet.Get());

        auto set = ComPtr<IDWriteFontSet>();

        if (FAILED(builder->CreateFontSet(set.GetAddressOf())))
            return false;

        auto built = ComPtr<IDWriteFontCollection1>();

        if (FAILED(factory->CreateFontCollectionFromFontSet(set.Get(),
                                                            built.GetAddressOf())))
            return false;

        current = built;

        return true;
    }

    std::mutex mutex;
    ComPtr<IDWriteInMemoryFontFileLoader> loader;
    std::vector<ComPtr<IDWriteFontFile>> files;
    ComPtr<IDWriteFontCollection> current;
};

FontRegistry& fontRegistry()
{
    static auto registry = FontRegistry {};
    return registry;
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

// The text MapCharacters analyses. It is a single codepoint on the stack that
// never outlives the call, so the reference counting is deliberately inert.
class SingleGlyphSource final : public IDWriteTextAnalysisSource
{
public:
    SingleGlyphSource(const wchar_t* textToUse, UINT32 lengthToUse)
        : text(textToUse)
        , length(lengthToUse)
    {
    }

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID id, void** object) override
    {
        if (id == __uuidof(IUnknown) || id == __uuidof(IDWriteTextAnalysisSource))
        {
            *object = this;
            return S_OK;
        }

        *object = nullptr;
        return E_NOINTERFACE;
    }

    ULONG STDMETHODCALLTYPE AddRef() override { return 1; }
    ULONG STDMETHODCALLTYPE Release() override { return 1; }

    HRESULT STDMETHODCALLTYPE GetTextAtPosition(UINT32 position,
                                                const WCHAR** textOut,
                                                UINT32* lengthOut) override
    {
        const auto inside = position < length;

        *textOut = inside ? text + position : nullptr;
        *lengthOut = inside ? length - position : 0;

        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE GetTextBeforePosition(UINT32 position,
                                                    const WCHAR** textOut,
                                                    UINT32* lengthOut) override
    {
        const auto inside = position > 0 && position <= length;

        *textOut = inside ? text : nullptr;
        *lengthOut = inside ? position : 0;

        return S_OK;
    }

    DWRITE_READING_DIRECTION STDMETHODCALLTYPE
        GetParagraphReadingDirection() override
    {
        return DWRITE_READING_DIRECTION_LEFT_TO_RIGHT;
    }

    HRESULT STDMETHODCALLTYPE GetLocaleName(UINT32 position,
                                            UINT32* lengthOut,
                                            const WCHAR** nameOut) override
    {
        *lengthOut = length - std::min(position, length);
        *nameOut = nullptr;

        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE
        GetNumberSubstitution(UINT32 position,
                              UINT32* lengthOut,
                              IDWriteNumberSubstitution** substitution) override
    {
        *lengthOut = length - std::min(position, length);
        *substitution = nullptr;

        return S_OK;
    }

private:
    const wchar_t* text;
    UINT32 length;
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

        collection = fontRegistry().collection();

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

    // Nothing on Windows is called Menlo or SF Mono. Rather than refuse to draw,
    // substitute the way CoreText does for an unknown name — a face that exists
    // beats no text at all, and callers wanting a specific one ask for a family
    // the platform ships.
    std::wstring resolveFamily() const
    {
        const auto wanted = toWideString(request.family);

        if (hasFamily(wanted))
            return wanted;

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
    // family shipping only Regular answers a bold request with Regular. Ask
    // DirectWrite to synthesize whatever the family does not have, which is what
    // CTFontCreateCopyWithSymbolicTraits does on the Apple side.
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

    // A codepoint resolved to the face that can actually draw it.
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

    // How a Latin family still renders CJK and emoji: hand the codepoint to the
    // system fallback and use whichever face it names.
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

        // The fallback face can be drawn at a different em to stay visually
        // matched to the base font; MapCharacters reports the factor.
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
    return fontRegistry().add(data, size);
}
} // namespace eacp::Text
