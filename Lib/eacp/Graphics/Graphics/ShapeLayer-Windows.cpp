// Windows implementation of ShapeLayer using Direct2D
#include "ShapeLayer.h"
#include "NativeLayer-Windows.h"

#include <cassert>

#define NOMINMAX
#include <Windows.h>
#include <d2d1.h>
#include <wrl/client.h>

namespace eacp::Graphics
{

using Microsoft::WRL::ComPtr;

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
}

void ShapeLayer::clearPath()
{
    impl->pathGeometry.Reset();
}

void ShapeLayer::setFillColor(const Color& color)
{
    impl->fillColor = color;
    impl->useGradient = false;
    impl->hasFill = true;
}

void ShapeLayer::setFillGradient(const LinearGradient& gradient)
{
    impl->gradient = gradient;
    impl->useGradient = true;
    impl->hasFill = true;
}

void ShapeLayer::setStrokeColor(const Color& color)
{
    impl->strokeColor = color;
    impl->hasStroke = true;
}

void ShapeLayer::setStrokeWidth(float width)
{
    impl->strokeWidth = width;
}

void* ShapeLayer::getNativeLayer()
{
    return impl.get();
}

} // namespace eacp::Graphics
