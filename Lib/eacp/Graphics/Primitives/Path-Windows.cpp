// Windows implementation of Path using Direct2D
#include "Path.h"

#include <cassert>

#define NOMINMAX
#include <Windows.h>
#include <d2d1.h>
#include <wrl/client.h>

// Forward declaration of factory access
namespace eacp::Graphics
{
ID2D1Factory* getD2DFactory();
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
        impl->sink->AddQuadraticBezier(
            D2D1::QuadraticBezierSegment(D2D1::Point2F(cx, cy), D2D1::Point2F(x, y)));
    }
    impl->lastPoint = {x, y};
}

void Path::cubicTo(float c1x, float c1y, float c2x, float c2y, float x, float y)
{
    impl->ensureFigureStarted();
    if (impl->sink)
    {
        impl->sink->AddBezier(D2D1::BezierSegment(D2D1::Point2F(c1x, c1y),
                                                   D2D1::Point2F(c2x, c2y),
                                                   D2D1::Point2F(x, y)));
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
        impl->sink->AddArc(D2D1::ArcSegment(
            D2D1::Point2F(x + w, y + r), D2D1::SizeF(r, r), 0.0f,
            D2D1_SWEEP_DIRECTION_CLOCKWISE, D2D1_ARC_SIZE_SMALL));
    }
    impl->lastPoint = {x + w, y + r};

    // Right edge
    lineTo({x + w, y + h - r});

    // Bottom-right arc
    if (impl->sink)
    {
        impl->sink->AddArc(D2D1::ArcSegment(
            D2D1::Point2F(x + w - r, y + h), D2D1::SizeF(r, r), 0.0f,
            D2D1_SWEEP_DIRECTION_CLOCKWISE, D2D1_ARC_SIZE_SMALL));
    }
    impl->lastPoint = {x + w - r, y + h};

    // Bottom edge
    lineTo({x + r, y + h});

    // Bottom-left arc
    if (impl->sink)
    {
        impl->sink->AddArc(D2D1::ArcSegment(D2D1::Point2F(x, y + h - r),
                                            D2D1::SizeF(r, r), 0.0f,
                                            D2D1_SWEEP_DIRECTION_CLOCKWISE,
                                            D2D1_ARC_SIZE_SMALL));
    }
    impl->lastPoint = {x, y + h - r};

    // Left edge
    lineTo({x, y + r});

    // Top-left arc
    if (impl->sink)
    {
        impl->sink->AddArc(D2D1::ArcSegment(D2D1::Point2F(x + r, y), D2D1::SizeF(r, r),
                                            0.0f, D2D1_SWEEP_DIRECTION_CLOCKWISE,
                                            D2D1_ARC_SIZE_SMALL));
    }
    impl->lastPoint = {x + r, y};

    close();
}

void Path::addEllipse(const Rect& rect)
{
    float cx = rect.x + rect.w / 2.0f;
    float cy = rect.y + rect.h / 2.0f;
    float rx = rect.w / 2.0f;
    float ry = rect.h / 2.0f;

    // Start at the right side of the ellipse
    moveTo({cx + rx, cy});

    // Draw ellipse using two arcs
    if (impl->sink)
    {
        // Bottom half
        impl->sink->AddArc(D2D1::ArcSegment(D2D1::Point2F(cx - rx, cy),
                                            D2D1::SizeF(rx, ry), 0.0f,
                                            D2D1_SWEEP_DIRECTION_CLOCKWISE,
                                            D2D1_ARC_SIZE_SMALL));
        // Top half
        impl->sink->AddArc(D2D1::ArcSegment(D2D1::Point2F(cx + rx, cy),
                                            D2D1::SizeF(rx, ry), 0.0f,
                                            D2D1_SWEEP_DIRECTION_CLOCKWISE,
                                            D2D1_ARC_SIZE_SMALL));
    }
    impl->lastPoint = {cx + rx, cy};

    close();
}

void* Path::getHandle() const
{
    return impl->getGeometry();
}

} // namespace eacp::Graphics
