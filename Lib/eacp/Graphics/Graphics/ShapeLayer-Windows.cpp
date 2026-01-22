// Windows implementation of ShapeLayer using Direct2D
#include "ShapeLayer.h"

#include <cassert>

#define NOMINMAX
#include <Windows.h>
#include <d2d1.h>
#include <wrl/client.h>

namespace eacp::Graphics
{

using Microsoft::WRL::ComPtr;

struct ShapeLayer::Native
{
    Native() {}

    // Path geometry (reference, not owned)
    ID2D1PathGeometry* pathGeometry = nullptr;

    // Fill settings
    Color fillColor;
    LinearGradient gradient;
    bool useGradient = false;
    bool hasFill = false;

    // Stroke settings
    Color strokeColor;
    float strokeWidth = 1.0f;
    bool hasStroke = false;

    // Layer properties
    Rect bounds;
    Point position;
    float opacity = 1.0f;
    bool hidden = false;
};

ShapeLayer::ShapeLayer()
    : impl()
{
}

void ShapeLayer::setPath(const Path& path)
{
    impl->pathGeometry = static_cast<ID2D1PathGeometry*>(path.getHandle());
}

void ShapeLayer::clearPath()
{
    impl->pathGeometry = nullptr;
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
    return &impl;
}

} // namespace eacp::Graphics
