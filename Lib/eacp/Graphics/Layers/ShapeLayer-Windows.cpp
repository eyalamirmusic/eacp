// Windows implementation of ShapeLayer using Windows.UI.Composition surfaces
#include "ShapeLayer.h"
#include "NativeLayer-Windows.h"

#include <cassert>
#include <algorithm>

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#include <d2d1_1.h>
#include <wrl/client.h>

#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.UI.Composition.h>
#include <windows.ui.composition.interop.h>

namespace wuc = winrt::Windows::UI::Composition;

namespace eacp::Graphics
{

using Microsoft::WRL::ComPtr;

// Forward declaration
ID2D1Factory1* getD2DFactory();

struct ShapeLayer::Native : NativeLayerBase
{
    // Path geometry (owned - AddRef'd to keep alive)
    ComPtr<ID2D1PathGeometry> pathGeometry;

    // Fill settings
    Color fillColor;
    LinearGradient gradient;
    bool useGradient = false;
    bool hasFill = false;

    // Stroke settings
    Color strokeColor;
    float strokeWidth = 1.0f;
    bool hasStroke = false;

    void renderContent() override
    {
        if (!surface || !pathGeometry)
            return;

        if (!hasFill && !hasStroke)
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

        // Base transform: offset + DPI scale
        auto baseTransform = D2D1::Matrix3x2F::Scale(dpiScale, dpiScale)
                             * D2D1::Matrix3x2F::Translation(
                                 static_cast<float>(offset.x),
                                 static_cast<float>(offset.y));

        // Fill
        if (hasFill)
        {
            if (useGradient && !gradient.stops.empty())
            {
                // Create gradient brush
                D2D1_GRADIENT_STOP stops[8];
                UINT32 stopCount = static_cast<UINT32>(
                    (std::min)(gradient.stops.size(), size_t(8)));

                for (UINT32 i = 0; i < stopCount; ++i)
                {
                    auto& stop = gradient.stops[i];
                    stops[i].position = stop.position;
                    stops[i].color = D2D1::ColorF(
                        stop.color.r, stop.color.g, stop.color.b, stop.color.a);
                }

                ComPtr<ID2D1GradientStopCollection> stopCollection;
                dc->CreateGradientStopCollection(
                    stops, stopCount, stopCollection.GetAddressOf());

                if (stopCollection)
                {
                    ComPtr<ID2D1LinearGradientBrush> gradientBrush;
                    dc->CreateLinearGradientBrush(
                        D2D1::LinearGradientBrushProperties(
                            D2D1::Point2F(gradient.start.x, gradient.start.y),
                            D2D1::Point2F(gradient.end.x, gradient.end.y)),
                        stopCollection.Get(),
                        gradientBrush.GetAddressOf());

                    if (gradientBrush)
                    {
                        dc->SetTransform(baseTransform);
                        dc->FillGeometry(pathGeometry.Get(), gradientBrush.Get());
                    }
                }
            }
            else
            {
                // Solid color fill
                ComPtr<ID2D1SolidColorBrush> brush;
                dc->CreateSolidColorBrush(
                    D2D1::ColorF(fillColor.r, fillColor.g, fillColor.b, fillColor.a),
                    brush.GetAddressOf());

                if (brush)
                {
                    dc->SetTransform(baseTransform);
                    dc->FillGeometry(pathGeometry.Get(), brush.Get());
                }
            }
        }

        // Stroke
        if (hasStroke && strokeWidth > 0)
        {
            ComPtr<ID2D1SolidColorBrush> strokeBrush;
            dc->CreateSolidColorBrush(
                D2D1::ColorF(
                    strokeColor.r, strokeColor.g, strokeColor.b, strokeColor.a),
                strokeBrush.GetAddressOf());

            if (strokeBrush)
            {
                dc->SetTransform(baseTransform);
                dc->DrawGeometry(
                    pathGeometry.Get(), strokeBrush.Get(), strokeWidth * dpiScale);
            }
        }

        dc->SetTransform(D2D1::Matrix3x2F::Identity());
        interop->EndDraw();
    }
};

ShapeLayer::ShapeLayer()
    : impl()
{
}

void ShapeLayer::setPath(const Path& path)
{
    auto* geometry = static_cast<ID2D1PathGeometry*>(path.getHandle());
    if (geometry)
    {
        // AddRef to keep geometry alive even after Path object is destroyed
        geometry->AddRef();
        impl->pathGeometry.Attach(geometry);
    }
    else
    {
        impl->pathGeometry.Reset();
    }
    impl->markContentDirty();
}

void ShapeLayer::clearPath()
{
    impl->pathGeometry.Reset();
    impl->markContentDirty();
}

void ShapeLayer::setFillColor(const Color& color)
{
    impl->fillColor = color;
    impl->useGradient = false;
    impl->hasFill = true;
    impl->markContentDirty();
}

void ShapeLayer::setFillGradient(const LinearGradient& gradient)
{
    impl->gradient = gradient;
    impl->useGradient = true;
    impl->hasFill = true;
    impl->markContentDirty();
}

void ShapeLayer::setStrokeColor(const Color& color)
{
    impl->strokeColor = color;
    impl->hasStroke = true;
    impl->markContentDirty();
}

void ShapeLayer::setStrokeWidth(float width)
{
    impl->strokeWidth = width;
    impl->markContentDirty();
}

void* ShapeLayer::getNativeLayer()
{
    return impl.get();
}

} // namespace eacp::Graphics
