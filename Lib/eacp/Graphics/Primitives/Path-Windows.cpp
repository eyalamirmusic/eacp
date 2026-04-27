// Windows implementation of Path using Direct2D
#include "Path.h"

#include <eacp/Core/Utils/Windows.h>

#include <cassert>

#include <d2d1_1.h>
#include <wrl/client.h>

// Forward declaration of factory access
namespace eacp::Graphics
{
ID2D1Factory1* getD2DFactory();
}

namespace eacp::Graphics
{

using Microsoft::WRL::ComPtr;

struct Path::Native
{
    Native() { rebuildGeometry(); }

    void rebuildGeometry()
    {
        geometry.Reset();
        sink.Reset();

        auto* factory = getD2DFactory();
        if (factory)
        {
            factory->CreatePathGeometry(geometry.GetAddressOf());
            if (geometry)
            {
                geometry->Open(sink.GetAddressOf());
                figureOpen = false;
            }
        }
    }

    void ensureFigureStarted()
    {
        if (sink && !figureOpen)
        {
            sink->BeginFigure(D2D1::Point2F(lastPoint.x, lastPoint.y),
                              D2D1_FIGURE_BEGIN_FILLED);
            figureOpen = true;
        }
    }

    void closeSinkIfNeeded()
    {
        if (sink)
        {
            if (figureOpen)
            {
                sink->EndFigure(D2D1_FIGURE_END_OPEN);
                figureOpen = false;
            }
            sink->Close();
            sink.Reset();
        }
    }

    ID2D1PathGeometry* getGeometry() const
    {
        // Note: This cast away const is safe because we're just finalizing the geometry
        const_cast<Native*>(this)->closeSinkIfNeeded();
        return geometry.Get();
    }

    ComPtr<ID2D1PathGeometry> geometry;
    ComPtr<ID2D1GeometrySink> sink;
    Point lastPoint;
    bool figureOpen = false;
};

Path::Path()
    : impl()
{
}

void Path::clear()
{
    impl->rebuildGeometry();
    impl->lastPoint = {};
}

void Path::moveTo(const Point& target)
{
    if (impl->sink)
    {
        if (impl->figureOpen)
        {
            impl->sink->EndFigure(D2D1_FIGURE_END_OPEN);
        }
        impl->sink->BeginFigure(D2D1::Point2F(target.x, target.y),
                                D2D1_FIGURE_BEGIN_FILLED);
        impl->figureOpen = true;
    }
    impl->lastPoint = target;
}

void Path::lineTo(const Point& target)
{
    impl->ensureFigureStarted();
    if (impl->sink)
    {
        impl->sink->AddLine(D2D1::Point2F(target.x, target.y));
    }
    impl->lastPoint = target;
}

void Path::quadTo(float cx, float cy, float x, float y)
{
    impl->ensureFigureStarted();
    if (impl->sink)
    {
        impl->sink->AddQuadraticBezier(D2D1::QuadraticBezierSegment(
            D2D1::Point2F(cx, cy), D2D1::Point2F(x, y)));
    }
    impl->lastPoint = {x, y};
}

void Path::cubicTo(float c1x, float c1y, float c2x, float c2y, float x, float y)
{
    impl->ensureFigureStarted();
    if (impl->sink)
    {
        impl->sink->AddBezier(D2D1::BezierSegment(
            D2D1::Point2F(c1x, c1y), D2D1::Point2F(c2x, c2y), D2D1::Point2F(x, y)));
    }
    impl->lastPoint = {x, y};
}

void Path::close()
{
    if (impl->sink && impl->figureOpen)
    {
        impl->sink->EndFigure(D2D1_FIGURE_END_CLOSED);
        impl->figureOpen = false;
    }
}

void Path::addRect(const Rect& rect)
{
    moveTo({rect.x, rect.y});
    lineTo({rect.x + rect.w, rect.y});
    lineTo({rect.x + rect.w, rect.y + rect.h});
    lineTo({rect.x, rect.y + rect.h});
    close();
}

void Path::addRoundedRect(const Rect& rect, float radius)
{
    float r = radius;
    float x = rect.x;
    float y = rect.y;
    float w = rect.w;
    float h = rect.h;

    // Start at top-left after the arc
    moveTo({x + r, y});

    // Top edge
    lineTo({x + w - r, y});

    // Top-right arc
    if (impl->sink)
    {
        impl->sink->AddArc(D2D1::ArcSegment(D2D1::Point2F(x + w, y + r),
                                            D2D1::SizeF(r, r),
                                            0.0f,
                                            D2D1_SWEEP_DIRECTION_CLOCKWISE,
                                            D2D1_ARC_SIZE_SMALL));
    }
    impl->lastPoint = {x + w, y + r};

    // Right edge
    lineTo({x + w, y + h - r});

    // Bottom-right arc
    if (impl->sink)
    {
        impl->sink->AddArc(D2D1::ArcSegment(D2D1::Point2F(x + w - r, y + h),
                                            D2D1::SizeF(r, r),
                                            0.0f,
                                            D2D1_SWEEP_DIRECTION_CLOCKWISE,
                                            D2D1_ARC_SIZE_SMALL));
    }
    impl->lastPoint = {x + w - r, y + h};

    // Bottom edge
    lineTo({x + r, y + h});

    // Bottom-left arc
    if (impl->sink)
    {
        impl->sink->AddArc(D2D1::ArcSegment(D2D1::Point2F(x, y + h - r),
                                            D2D1::SizeF(r, r),
                                            0.0f,
                                            D2D1_SWEEP_DIRECTION_CLOCKWISE,
                                            D2D1_ARC_SIZE_SMALL));
    }
    impl->lastPoint = {x, y + h - r};

    // Left edge
    lineTo({x, y + r});

    // Top-left arc
    if (impl->sink)
    {
        impl->sink->AddArc(D2D1::ArcSegment(D2D1::Point2F(x + r, y),
                                            D2D1::SizeF(r, r),
                                            0.0f,
                                            D2D1_SWEEP_DIRECTION_CLOCKWISE,
                                            D2D1_ARC_SIZE_SMALL));
    }
    impl->lastPoint = {x + r, y};

    close();
}

void Path::addEllipse(const Rect& rect)
{
    auto cx = rect.x + rect.w / 2.0f;
    auto cy = rect.y + rect.h / 2.0f;
    auto rx = rect.w / 2.0f;
    auto ry = rect.h / 2.0f;

    moveTo({cx + rx, cy});

    auto sweepQuadrant = [&](float endX, float endY)
    {
        if (impl->sink)
        {
            impl->sink->AddArc(D2D1::ArcSegment(D2D1::Point2F(endX, endY),
                                                D2D1::SizeF(rx, ry),
                                                0.0f,
                                                D2D1_SWEEP_DIRECTION_CLOCKWISE,
                                                D2D1_ARC_SIZE_SMALL));
        }
        impl->lastPoint = {endX, endY};
    };

    sweepQuadrant(cx, cy + ry);
    sweepQuadrant(cx - rx, cy);
    sweepQuadrant(cx, cy - ry);
    sweepQuadrant(cx + rx, cy);

    close();
}

Path Path::scaled(float sx, float sy) const
{
    auto* sourceGeometry = impl->getGeometry();
    if (!sourceGeometry)
        return {};

    auto* factory = getD2DFactory();
    if (!factory)
        return {};

    auto transform =
        D2D1::Matrix3x2F::Scale(D2D1::SizeF(sx, sy), D2D1::Point2F(0, 0));

    ComPtr<ID2D1TransformedGeometry> transformed;
    factory->CreateTransformedGeometry(
        sourceGeometry, transform, transformed.GetAddressOf());
    if (!transformed)
        return {};

    Path result;
    result.impl->closeSinkIfNeeded();
    result.impl->geometry.Reset();
    result.impl->sink.Reset();

    ComPtr<ID2D1PathGeometry> newGeometry;
    factory->CreatePathGeometry(newGeometry.GetAddressOf());
    if (!newGeometry)
        return {};

    ComPtr<ID2D1GeometrySink> newSink;
    newGeometry->Open(newSink.GetAddressOf());
    if (!newSink)
        return {};

    transformed->Simplify(D2D1_GEOMETRY_SIMPLIFICATION_OPTION_CUBICS_AND_LINES,
                          nullptr,
                          newSink.Get());
    newSink->Close();

    result.impl->geometry = newGeometry;
    return result;
}

void* Path::getHandle() const
{
    return impl->getGeometry();
}

} // namespace eacp::Graphics
