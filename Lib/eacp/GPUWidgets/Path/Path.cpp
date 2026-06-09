#include "Path.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include "../Common.h"

namespace eacp::GPUWidgets
{
namespace
{
// Curves and arcs flatten to this many line segments. A fixed subdivision is
// plenty for the shapes this version draws; flatness-adaptive subdivision is a
// future refinement.
constexpr int curveSegments = 24;

Graphics::Point lerp(const Graphics::Point& a, const Graphics::Point& b, float t)
{
    return {a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t};
}
} // namespace

void Path::clear()
{
    subPaths.clear();
    currentPoint = {};
}

bool Path::isEmpty() const
{
    return subPaths.empty();
}

Path::SubPath& Path::currentSubPath()
{
    // A line/curve before any moveTo seeds a sub-path at the current point so its
    // first vertex is not lost.
    if (subPaths.empty())
    {
        auto seeded = SubPath {};
        seeded.points.add(currentPoint);
        subPaths.add(std::move(seeded));
    }

    return subPaths.back();
}

void Path::moveTo(const Graphics::Point& target)
{
    auto sub = SubPath {};
    sub.points.add(target);
    subPaths.add(std::move(sub));
    currentPoint = target;
}

void Path::lineTo(const Graphics::Point& target)
{
    currentSubPath().points.add(target);
    currentPoint = target;
}

void Path::quadTo(float controlX, float controlY, float endX, float endY)
{
    auto start = currentPoint;
    auto control = Graphics::Point {controlX, controlY};
    auto end = Graphics::Point {endX, endY};
    auto& points = currentSubPath().points;

    for (auto i = 1; i <= curveSegments; ++i)
    {
        auto t = (float) i / (float) curveSegments;
        auto a = lerp(start, control, t);
        auto b = lerp(control, end, t);
        points.add(lerp(a, b, t));
    }

    currentPoint = end;
}

void Path::cubicTo(float control1X,
                   float control1Y,
                   float control2X,
                   float control2Y,
                   float endX,
                   float endY)
{
    auto p0 = currentPoint;
    auto p1 = Graphics::Point {control1X, control1Y};
    auto p2 = Graphics::Point {control2X, control2Y};
    auto p3 = Graphics::Point {endX, endY};
    auto& points = currentSubPath().points;

    for (auto i = 1; i <= curveSegments; ++i)
    {
        auto t = (float) i / (float) curveSegments;
        auto a = lerp(p0, p1, t);
        auto b = lerp(p1, p2, t);
        auto c = lerp(p2, p3, t);
        auto d = lerp(a, b, t);
        auto e = lerp(b, c, t);
        points.add(lerp(d, e, t));
    }

    currentPoint = p3;
}

void Path::close()
{
    if (!subPaths.empty())
        subPaths.back().closed = true;
}

void Path::addRect(const Graphics::Rect& rect)
{
    auto sub = SubPath {};
    sub.points.add({rect.x, rect.y});
    sub.points.add({rect.x + rect.w, rect.y});
    sub.points.add({rect.x + rect.w, rect.y + rect.h});
    sub.points.add({rect.x, rect.y + rect.h});
    sub.closed = true;

    subPaths.add(std::move(sub));
    currentPoint = {rect.x, rect.y};
}

void Path::addRoundedRect(const Graphics::Rect& rect, float cornerRadius)
{
    auto radius = std::min(cornerRadius, std::min(rect.w, rect.h) * 0.5f);

    if (radius <= 0.0f)
    {
        addRect(rect);
        return;
    }

    auto sub = SubPath {};
    auto cornerSegments = std::max(2, curveSegments / 3);

    // Sweeps a quarter-circle of the given corner, appending its points. Angles
    // run in a y-down screen space, so the four corners trace the outline in
    // order with the straight edges formed implicitly between them.
    auto addArc = [&](float centerX, float centerY, float startAngle)
    {
        for (auto i = 0; i <= cornerSegments; ++i)
        {
            auto angle =
                startAngle + (float) i / (float) cornerSegments * (pi * 0.5f);
            sub.points.add({centerX + std::cos(angle) * radius,
                            centerY + std::sin(angle) * radius});
        }
    };

    auto left = rect.x + radius;
    auto right = rect.x + rect.w - radius;
    auto top = rect.y + radius;
    auto bottom = rect.y + rect.h - radius;

    addArc(left, top, pi); // top-left
    addArc(right, top, pi * 1.5f); // top-right
    addArc(right, bottom, 0.0f); // bottom-right
    addArc(left, bottom, pi * 0.5f); // bottom-left

    sub.closed = true;
    subPaths.add(std::move(sub));
    currentPoint = {rect.x, top};
}

void Path::addEllipse(const Graphics::Rect& rect)
{
    auto centerX = rect.x + rect.w * 0.5f;
    auto centerY = rect.y + rect.h * 0.5f;
    auto radiusX = rect.w * 0.5f;
    auto radiusY = rect.h * 0.5f;
    auto segments = curveSegments * 2;

    auto sub = SubPath {};

    for (auto i = 0; i < segments; ++i)
    {
        auto angle = (float) i / (float) segments * 2.0f * pi;
        sub.points.add({centerX + std::cos(angle) * radiusX,
                        centerY + std::sin(angle) * radiusY});
    }

    sub.closed = true;
    subPaths.add(std::move(sub));
    currentPoint = {centerX + radiusX, centerY};
}

Graphics::Rect Path::getBounds() const
{
    if (isEmpty())
        return {};

    auto minX = std::numeric_limits<float>::max();
    auto minY = std::numeric_limits<float>::max();
    auto maxX = std::numeric_limits<float>::lowest();
    auto maxY = std::numeric_limits<float>::lowest();

    for (const auto& sub: subPaths)
    {
        for (const auto& point: sub.points)
        {
            minX = std::min(minX, point.x);
            minY = std::min(minY, point.y);
            maxX = std::max(maxX, point.x);
            maxY = std::max(maxY, point.y);
        }
    }

    return {minX, minY, maxX - minX, maxY - minY};
}
} // namespace eacp::GPUWidgets
