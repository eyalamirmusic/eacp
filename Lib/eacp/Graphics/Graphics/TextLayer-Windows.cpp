// Windows implementation of TextLayer using DirectComposition surfaces
#include "TextLayer.h"
#include "NativeLayer-Windows.h"

#include <cassert>

#define NOMINMAX
#include <Windows.h>
#include <d2d1_1.h>
#include <dwrite.h>
#include <wrl/client.h>

// Forward declaration of factory access
namespace eacp::Graphics
{
IDWriteFactory* getDWriteFactory();
}

namespace eacp::Graphics
{

using Microsoft::WRL::ComPtr;

struct TextLayer::Native : NativeLayerBase
{
    Native()
    {
        // Create default text format
        auto* factory = getDWriteFactory();
        if (factory)
        {
            factory->CreateTextFormat(L"Arial", nullptr, DWRITE_FONT_WEIGHT_NORMAL,
                                      DWRITE_FONT_STYLE_NORMAL,
                                      DWRITE_FONT_STRETCH_NORMAL, 12.0f, L"en-us",
                                      textFormat.GetAddressOf());
        }
    }

    std::wstring text;
    ComPtr<IDWriteTextFormat> textFormat;
    Color color {1.0f, 1.0f, 1.0f, 1.0f};

    void renderContent() override
    {
        if (!surface || text.empty() || !textFormat)
            return;

        POINT offset;
        ComPtr<ID2D1DeviceContext> dc;
        int width = static_cast<int>(bounds.w);
        int height = static_cast<int>(bounds.h);

        if (width <= 0 || height <= 0)
            return;

        RECT updateRect = {0, 0, width, height};

        HRESULT hr = surface->BeginDraw(&updateRect, IID_PPV_ARGS(&dc), &offset);
        if (FAILED(hr) || !dc)
            return;

        // Clear with transparent background
        dc->Clear(D2D1::ColorF(0, 0, 0, 0));

        // Create brush for text
        ComPtr<ID2D1SolidColorBrush> brush;
        dc->CreateSolidColorBrush(
            D2D1::ColorF(color.r, color.g, color.b, color.a), brush.GetAddressOf());

        if (brush)
        {
            D2D1_RECT_F layoutRect = D2D1::RectF(
                static_cast<float>(offset.x), static_cast<float>(offset.y),
                static_cast<float>(offset.x) + bounds.w,
                static_cast<float>(offset.y) + bounds.h);

            dc->DrawText(text.c_str(), static_cast<UINT32>(text.length()),
                         textFormat.Get(), layoutRect, brush.Get());
        }

        surface->EndDraw();
    }
};

TextLayer::TextLayer()
    : impl()
{
}

void TextLayer::setText(const std::string& text)
{
    // Convert to wide string
    int len = MultiByteToWideChar(CP_UTF8, 0, text.c_str(), -1, nullptr, 0);
    impl->text.resize(len - 1); // Don't include null terminator in string
    MultiByteToWideChar(CP_UTF8, 0, text.c_str(), -1, impl->text.data(), len);
    impl->markContentDirty();
}

void TextLayer::setFont(const Font& font)
{
    auto* textFormat = static_cast<IDWriteTextFormat*>(font.getHandle());
    if (textFormat)
    {
        // AddRef to keep the text format alive
        textFormat->AddRef();
        impl->textFormat.Attach(textFormat);
    }
    impl->markContentDirty();
}

void TextLayer::setColor(const Color& color)
{
    impl->color = color;
    impl->markContentDirty();
}

void* TextLayer::getNativeLayer()
{
    return impl.get();
}

} // namespace eacp::Graphics
