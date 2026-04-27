// Windows implementation of TextLayer using Windows.UI.Composition surfaces
#include "TextLayer.h"
#include "NativeLayer-Windows.h"
#include "../Helpers/StringUtils-Windows.h"

#include <eacp/Core/Utils/Windows.h>

#include <cassert>

#include <d2d1_1.h>
#include <dwrite.h>
#include <wrl/client.h>

#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.UI.Composition.h>
#include <windows.ui.composition.interop.h>

namespace wuc = winrt::Windows::UI::Composition;

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
            factory->CreateTextFormat(L"Arial",
                                      nullptr,
                                      DWRITE_FONT_WEIGHT_NORMAL,
                                      DWRITE_FONT_STYLE_NORMAL,
                                      DWRITE_FONT_STRETCH_NORMAL,
                                      12.0f,
                                      L"en-us",
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

        if (bounds.w <= 0 || bounds.h <= 0)
            return;

        auto dpiScale = getDpiScale();
        auto surfaceWidth = static_cast<int>(bounds.w * dpiScale);
        auto surfaceHeight = static_cast<int>(bounds.h * dpiScale);

        // Get interop interface for BeginDraw/EndDraw
        auto interop = surface.as<
            ABI::Windows::UI::Composition::ICompositionDrawingSurfaceInterop>();
        if (!interop)
            return;

        POINT offset;
        winrt::com_ptr<ID2D1DeviceContext> dc;
        RECT updateRect = {0, 0, surfaceWidth, surfaceHeight};

        HRESULT hr =
            interop->BeginDraw(&updateRect, IID_PPV_ARGS(dc.put()), &offset);
        if (FAILED(hr) || !dc)
            return;

        // Clear with transparent background
        dc->Clear(D2D1::ColorF(0, 0, 0, 0));

        // Apply DPI scale transform
        auto baseTransform = D2D1::Matrix3x2F::Scale(dpiScale, dpiScale)
                             * D2D1::Matrix3x2F::Translation(
                                 static_cast<float>(offset.x),
                                 static_cast<float>(offset.y));
        dc->SetTransform(baseTransform);

        // Create brush for text
        ComPtr<ID2D1SolidColorBrush> brush;
        dc->CreateSolidColorBrush(D2D1::ColorF(color.r, color.g, color.b, color.a),
                                  brush.GetAddressOf());

        if (brush)
        {
            D2D1_RECT_F layoutRect = D2D1::RectF(0, 0, bounds.w, bounds.h);

            dc->DrawText(text.c_str(),
                         static_cast<UINT32>(text.length()),
                         textFormat.Get(),
                         layoutRect,
                         brush.Get());
        }

        dc->SetTransform(D2D1::Matrix3x2F::Identity());
        interop->EndDraw();
    }
};

TextLayer::TextLayer()
    : impl()
{
}

void TextLayer::setText(const std::string& text)
{
    impl->text = toWideString(text);
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
