#include "GlyphAtlas.h"
#include "TermTypes.h"

#include <eacp/Core/Utils/WinInclude.h>
#include <eacp/Graphics/Helpers/StringUtils-Windows.h>

#include <ResEmbed/ResEmbed.h>

#include <d2d1_1.h>
#include <dwrite_3.h>
#include <wincodec.h>
#include <wrl/client.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <optional>
#include <unordered_map>
#include <vector>

namespace eacp::Graphics
{
IDWriteFactory* getDWriteFactory();
ID2D1Factory1* getD2DFactory();
void registerInMemoryFont(const void* data, std::size_t bytes);
IDWriteFontCollection1* getApplicationFontCollection();
} // namespace eacp::Graphics

namespace term
{
using namespace eacp;
using Microsoft::WRL::ComPtr;

namespace
{
constexpr int atlasSize = 2048;
constexpr int slotPadding = 1;

std::uint32_t slotKey(char32_t cp, bool bold, bool italic)
{
    return (std::uint32_t) cp | (bold ? 1u << 22 : 0) | (italic ? 1u << 23 : 0);
}

int encodeUtf16(char32_t cp, wchar_t* units)
{
    if (cp <= 0xffff)
    {
        units[0] = (wchar_t) cp;
        return 1;
    }

    const auto v = cp - 0x10000;
    units[0] = (wchar_t) (0xd800 + (v >> 10));
    units[1] = (wchar_t) (0xdc00 + (v & 0x3ff));
    return 2;
}

IWICImagingFactory* wicFactory()
{
    static const auto factory = []
    {
        auto created = ComPtr<IWICImagingFactory> {};
        CoCreateInstance(CLSID_WICImagingFactory,
                         nullptr,
                         CLSCTX_INPROC_SERVER,
                         IID_PPV_ARGS(created.GetAddressOf()));
        return created;
    }();

    return factory.Get();
}

bool containsFamily(IDWriteFontCollection* collection, const std::wstring& family)
{
    if (collection == nullptr)
        return false;

    auto index = UINT32 {0};
    auto exists = BOOL {FALSE};
    collection->FindFamilyName(family.c_str(), &index, &exists);
    return exists != FALSE;
}
} // namespace

void registerEmbeddedFonts()
{
    static const auto once = []
    {
        for (const auto* name: {"JetBrainsMono-Regular.ttf",
                                "JetBrainsMono-Bold.ttf",
                                "JetBrainsMono-Italic.ttf",
                                "JetBrainsMono-BoldItalic.ttf"})
        {
            const auto resource = ResEmbed::get(name);

            if (resource.size() > 0)
                Graphics::registerInMemoryFont(resource.data(), resource.size());
        }

        return true;
    }();

    (void) once;
}

struct GlyphAtlas::Impl
{
    Impl(const std::string& fontName, float pointSizeToUse, float scaleToUse)
    {
        scale = scaleToUse > 0 ? scaleToUse : (float) GetDpiForSystem() / 96.0f;

        if (scale <= 0)
            scale = 1.0f;

        pointSize = pointSizeToUse;
        emSize = pointSize * scale;

        resolveFamily(fontName);
        measureCell();
        pixels.assign((std::size_t) atlasSize * atlasSize * 4, 0);
    }

    void resolveFamily(const std::string& requested)
    {
        auto* factory = Graphics::getDWriteFactory();

        if (factory == nullptr)
            return;

        familyName = Graphics::toWideString(requested);
        collection = Graphics::getApplicationFontCollection();

        if (containsFamily(collection.Get(), familyName))
            return;

        auto system = ComPtr<IDWriteFontCollection> {};
        factory->GetSystemFontCollection(system.GetAddressOf());
        collection = system;

        if (!containsFamily(collection.Get(), familyName))
            familyName = L"Consolas";
    }

    ComPtr<IDWriteFontFace> regularFace() const
    {
        if (!collection)
            return {};

        auto index = UINT32 {0};
        auto exists = BOOL {FALSE};
        collection->FindFamilyName(familyName.c_str(), &index, &exists);

        if (!exists)
            return {};

        auto family = ComPtr<IDWriteFontFamily> {};

        if (FAILED(collection->GetFontFamily(index, family.GetAddressOf())))
            return {};

        auto font = ComPtr<IDWriteFont> {};

        if (FAILED(family->GetFirstMatchingFont(DWRITE_FONT_WEIGHT_NORMAL,
                                                DWRITE_FONT_STRETCH_NORMAL,
                                                DWRITE_FONT_STYLE_NORMAL,
                                                font.GetAddressOf())))
            return {};

        auto face = ComPtr<IDWriteFontFace> {};
        font->CreateFontFace(face.GetAddressOf());
        return face;
    }

    void measureCell()
    {
        // Keep drawable defaults even when DirectWrite is unavailable (a
        // headless session): the view then still lays out sanely.
        ascentPx = emSize;
        cellPxW = (int) std::ceil(emSize * 0.6f);
        cellPxH = (int) std::ceil(emSize * 1.3f);

        const auto face = regularFace();

        if (!face)
            return;

        auto metrics = DWRITE_FONT_METRICS {};
        face->GetMetrics(&metrics);

        if (metrics.designUnitsPerEm == 0)
            return;

        const auto pxPerUnit = emSize / (float) metrics.designUnitsPerEm;

        ascentPx = (float) metrics.ascent * pxPerUnit;
        cellPxH = (int) std::ceil(
            (float) (metrics.ascent + metrics.descent + metrics.lineGap)
            * pxPerUnit);

        const auto format = makeFormat(false, false);

        if (const auto layout = makeLayout(L"M", 1, format.Get()))
        {
            auto text = DWRITE_TEXT_METRICS {};

            if (SUCCEEDED(layout->GetMetrics(&text))
                && text.widthIncludingTrailingWhitespace > 0)
                cellPxW = (int) std::ceil(text.widthIncludingTrailingWhitespace);
        }
    }

    ComPtr<IDWriteTextFormat> makeFormat(bool bold, bool italic) const
    {
        auto* factory = Graphics::getDWriteFactory();
        auto format = ComPtr<IDWriteTextFormat> {};

        if (factory == nullptr)
            return format;

        factory->CreateTextFormat(
            familyName.c_str(),
            collection.Get(),
            bold ? DWRITE_FONT_WEIGHT_BOLD : DWRITE_FONT_WEIGHT_NORMAL,
            italic ? DWRITE_FONT_STYLE_ITALIC : DWRITE_FONT_STYLE_NORMAL,
            DWRITE_FONT_STRETCH_NORMAL,
            emSize,
            L"en-us",
            format.GetAddressOf());
        return format;
    }

    ComPtr<IDWriteTextLayout>
        makeLayout(const wchar_t* units, int unitCount, IDWriteTextFormat* format)
    {
        auto* factory = Graphics::getDWriteFactory();
        auto layout = ComPtr<IDWriteTextLayout> {};

        if (factory == nullptr || format == nullptr)
            return layout;

        factory->CreateTextLayout(units,
                                  (UINT32) unitCount,
                                  format,
                                  emSize * 8.0f,
                                  emSize * 8.0f,
                                  layout.GetAddressOf());

        if (layout)
            layout->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);

        return layout;
    }

    const GlyphSlot& glyph(char32_t cp, bool bold, bool italic)
    {
        const auto key = slotKey(cp, bold, italic);
        auto found = slots.find(key);

        if (found != slots.end())
            return found->second;

        return slots.emplace(key, rasterize(cp, bold, italic)).first->second;
    }

    GlyphSlot rasterize(char32_t cp, bool bold, bool italic)
    {
        auto* factory = Graphics::getDWriteFactory();
        auto* d2d = Graphics::getD2DFactory();
        auto* wic = wicFactory();

        if (factory == nullptr || d2d == nullptr || wic == nullptr)
            return {};

        const auto cells = charWidth(cp) == 2 ? 2 : 1;
        const auto slotW = cellPxW * cells;
        const auto slotH = cellPxH;

        if (penX + slotW + slotPadding > atlasSize)
        {
            penX = slotPadding;
            penY += slotH + slotPadding;
        }

        if (penY + slotH + slotPadding > atlasSize)
            resetAtlas();

        wchar_t units[2] = {};
        const auto unitCount = encodeUtf16(cp, units);

        // A layout rather than a raw glyph run: it resolves system font
        // fallback (emoji, CJK, symbols) exactly like ordinary text drawing.
        const auto format = makeFormat(bold, italic);
        const auto layout = makeLayout(units, unitCount, format.Get());

        if (!layout)
            return {};

        auto line = DWRITE_LINE_METRICS {};
        auto lineCount = UINT32 {0};
        layout->GetLineMetrics(&line, 1, &lineCount);

        auto text = DWRITE_TEXT_METRICS {};
        layout->GetMetrics(&text);

        auto bitmap = ComPtr<IWICBitmap> {};

        if (FAILED(wic->CreateBitmap((UINT) slotW,
                                     (UINT) slotH,
                                     GUID_WICPixelFormat32bppPBGRA,
                                     WICBitmapCacheOnDemand,
                                     bitmap.GetAddressOf())))
            return {};

        const auto properties = D2D1::RenderTargetProperties(
            D2D1_RENDER_TARGET_TYPE_DEFAULT,
            D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM,
                              D2D1_ALPHA_MODE_PREMULTIPLIED),
            96.0f,
            96.0f);

        auto target = ComPtr<ID2D1RenderTarget> {};

        if (FAILED(d2d->CreateWicBitmapRenderTarget(
                bitmap.Get(), properties, target.GetAddressOf())))
            return {};

        // Grayscale so monochrome coverage lands in alpha (ClearType would
        // bake subpixel color into the atlas).
        target->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);

        auto brush = ComPtr<ID2D1SolidColorBrush> {};
        target->CreateSolidColorBrush(D2D1::ColorF(D2D1::ColorF::White),
                                      brush.GetAddressOf());

        if (!brush)
            return {};

        const auto advance = text.widthIncludingTrailingWhitespace;
        const auto x = std::max(((float) slotW - advance) / 2.0f, 0.0f);
        const auto y = ascentPx - line.baseline;

        target->BeginDraw();
        target->Clear(D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.0f));
        target->DrawTextLayout({x, y},
                               layout.Get(),
                               brush.Get(),
                               D2D1_DRAW_TEXT_OPTIONS_ENABLE_COLOR_FONT);

        if (FAILED(target->EndDraw()))
            return {};

        auto buffer = std::vector<std::uint8_t>(
            (std::size_t) slotW * (std::size_t) slotH * 4, 0);

        if (FAILED(bitmap->CopyPixels(
                nullptr, (UINT) slotW * 4, (UINT) buffer.size(), buffer.data())))
            return {};

        const auto colored = isColored(buffer);
        blitToAtlas(buffer, slotW, slotH, colored);

        auto slot = GlyphSlot {};
        slot.src = {(float) penX, (float) penY, (float) slotW, (float) slotH};
        slot.colored = colored;
        slot.valid = true;

        penX += slotW + slotPadding;
        dirty = true;
        return slot;
    }

    // White text premultiplied means every channel equals alpha; anything
    // else came from a color font (emoji palette layers).
    static bool isColored(const std::vector<std::uint8_t>& buffer)
    {
        for (auto index = std::size_t {0}; index < buffer.size(); index += 4)
        {
            const auto alpha = buffer[index + 3];

            for (auto channel = 0; channel < 3; ++channel)
                if (std::abs((int) buffer[index + channel] - (int) alpha) > 8)
                    return true;
        }

        return false;
    }

    // Converts the premultiplied BGRA render to the straight-alpha RGBA
    // atlas. Monochrome coverage becomes white + alpha for per-cell tinting.
    void blitToAtlas(const std::vector<std::uint8_t>& buffer,
                     int slotW,
                     int slotH,
                     bool colored)
    {
        for (auto y = 0; y < slotH; ++y)
        {
            const auto* src = &buffer[(std::size_t) y * (std::size_t) slotW * 4];
            auto* dst =
                &pixels[((std::size_t) (penY + y) * atlasSize + (std::size_t) penX)
                        * 4];

            for (auto x = 0; x < slotW; ++x)
            {
                const auto alpha = src[x * 4 + 3];

                if (colored && alpha > 0)
                {
                    dst[x * 4 + 0] =
                        (std::uint8_t) std::min(src[x * 4 + 2] * 255 / alpha, 255);
                    dst[x * 4 + 1] =
                        (std::uint8_t) std::min(src[x * 4 + 1] * 255 / alpha, 255);
                    dst[x * 4 + 2] =
                        (std::uint8_t) std::min(src[x * 4 + 0] * 255 / alpha, 255);
                }
                else
                {
                    dst[x * 4 + 0] = 255;
                    dst[x * 4 + 1] = 255;
                    dst[x * 4 + 2] = 255;
                }

                dst[x * 4 + 3] = alpha;
            }
        }
    }

    void resetAtlas()
    {
        std::fill(pixels.begin(), pixels.end(), std::uint8_t {0});
        slots.clear();
        penX = slotPadding;
        penY = slotPadding;
        dirty = true;
    }

    GPU::Texture& texture()
    {
        if (!tex)
        {
            auto descriptor = GPU::TextureDescriptor {};
            descriptor.width = atlasSize;
            descriptor.height = atlasSize;
            descriptor.format = GPU::TextureFormat::RGBA8Unorm;
            descriptor.filter = GPU::TextureFilter::Linear;
            tex.emplace(
                GPU::Device::shared().makeTexture(descriptor, pixels.data()));
            dirty = false;
        }
        else if (dirty)
        {
            tex->update(pixels.data());
            dirty = false;
        }

        return *tex;
    }

    float scale = 1;
    float pointSize = 13;
    float emSize = 13;
    float ascentPx = 0;
    int cellPxW = 0;
    int cellPxH = 0;
    int penX = slotPadding;
    int penY = slotPadding;
    bool dirty = true;

    std::wstring familyName;
    ComPtr<IDWriteFontCollection> collection;
    std::vector<std::uint8_t> pixels;
    std::unordered_map<std::uint32_t, GlyphSlot> slots;
    std::optional<GPU::Texture> tex;
};

GlyphAtlas::GlyphAtlas(const std::string& fontName, float pointSize, float scale)
    : impl(fontName, pointSize, scale)
{
}

GlyphAtlas::~GlyphAtlas() = default;

float GlyphAtlas::cellWidth() const
{
    return (float) impl->cellPxW / impl->scale;
}

float GlyphAtlas::cellHeight() const
{
    return (float) impl->cellPxH / impl->scale;
}

float GlyphAtlas::baseline() const
{
    return impl->ascentPx / impl->scale;
}

float GlyphAtlas::fontSize() const
{
    return impl->pointSize;
}

const GlyphSlot& GlyphAtlas::glyph(char32_t cp, bool bold, bool italic)
{
    return impl->glyph(cp, bold, italic);
}

GPU::Texture& GlyphAtlas::texture()
{
    return impl->texture();
}
} // namespace term
