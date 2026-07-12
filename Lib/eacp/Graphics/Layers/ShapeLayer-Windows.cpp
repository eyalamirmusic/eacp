// Windows implementation of ShapeLayer using DirectComposition surfaces
#include "ShapeLayer.h"
#include "NativeLayer-Windows.h"

#include <algorithm>

namespace eacp::Graphics
{

struct ShapeLayer::Native : NativeLayerBase
{
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

        POINT offset;
        ComPtr<ID2D1DeviceContext> dc;
        RECT updateRect = {0, 0, surfaceWidth, surfaceHeight};

        HRESULT hr = surface->BeginDraw(
            &updateRect, IID_PPV_ARGS(dc.GetAddressOf()), &offset);
        if (FAILED(hr) || !dc)
        {
            handleDeviceLossIfNeeded(hr);
            return;
        }

        dc->Clear(D2D1::ColorF(0, 0, 0, 0));

        auto baseTransform =
            D2D1::Matrix3x2F::Scale(dpiScale, dpiScale)
            * D2D1::Matrix3x2F::Translation(static_cast<float>(offset.x),
                                            static_cast<float>(offset.y));

        if (hasFill)
        {
            if (useGradient && !gradient.stops.empty())
            {
                D2D1_GRADIENT_STOP stops[8];
                auto stopCount = (std::min) (gradient.stops.size(), 8);

                for (auto i = 0; i < stopCount; ++i)
                {
                    auto& stop = gradient.stops[i];
                    stops[i].position = stop.position;
                    stops[i].color = D2D1::ColorF(
                        stop.color.r, stop.color.g, stop.color.b, stop.color.a);
                }

                ComPtr<ID2D1GradientStopCollection> stopCollection;
                dc->CreateGradientStopCollection(stops,
                                                 static_cast<UINT32>(stopCount),
                                                 stopCollection.GetAddressOf());

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
        surface->EndDraw();
    }

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
