#include "Path.h"
#include <algorithm>

#include "../Common.h"

namespace eacp::GPUWidgets
{
namespace
{
// Bounds the point list for degenerate control polygons.
constexpr int maxCurveSegments = 512;

Graphics::Point lerp(const Graphics::Point& a, const Graphics::Point& b, float t)
{
    return {a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t};
}

float length(const Graphics::Point& point)
{
    return std::sqrt(point.x * point.x + point.y * point.y);
}

// Bounds how far a Bezier bows away from the chord across the three points.
float secondDifference(const Graphics::Point& a,
                       const Graphics::Point& b,
                       const Graphics::Point& c)
{
    return length({a.x - 2.0f * b.x + c.x, a.y - 2.0f * b.y + c.y});
}
} // namespace

void Path::setFlatness(float toleranceInPathUnits)
{
    flatness = std::max(toleranceInPathUnits, epsilon);
}

// A Bezier in n uniform pieces strays by at most difference / (8 n^2).
int Path::segmentsForCurve(float difference) const
{
    auto needed = std::ceil(std::sqrt(difference / (8.0f * flatness)));
    return std::clamp((int) needed, 1, maxCurveSegments);
}

// An arc strays by its chord's sagitta, radius * (1 - cos(halfAngle)).
int Path::segmentsForArc(float radius, float sweep) const
{
    if (radius <= flatness)
        return 1;

    auto widest = std::acos(1.0f - flatness / radius);
    return std::clamp((int) std::ceil(sweep / (2.0f * widest)), 1, maxCurveSegments);
}

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
    // A line or curve before any moveTo starts at the current point.
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
    auto steps = segmentsForCurve(secondDifference(start, control, end));

    for (auto i = 1; i <= steps; ++i)
    {
        auto t = (float) i / (float) steps;
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

    // A cubic bows about either second difference, so the larger decides.
    auto steps = segmentsForCurve(
        std::max(secondDifference(p0, p1, p2), secondDifference(p1, p2, p3)));

    for (auto i = 1; i <= steps; ++i)
    {
        auto t = (float) i / (float) steps;
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
    auto cornerSegments = segmentsForArc(radius, pi * 0.5f);

    // Angles are y-down, so sweeping the corners in order traces the outline
    // with the straight edges implied between arcs.
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

    // One count for the whole sweep, from the longer axis: a per-quadrant count
    // would seam where it changed.
    auto segments = segmentsForArc(std::max(radiusX, radiusY), 2.0f * pi);

    // Outset by half the sagitta so the polygon straddles the true curve rather
    // than sitting wholly inside it, which would fill reliably undersized.
    auto sagitta = 1.0f - std::cos(pi / (float) segments);
    auto outsetX = radiusX * (1.0f + sagitta * 0.5f);
    auto outsetY = radiusY * (1.0f + sagitta * 0.5f);

    auto sub = SubPath {};

    for (auto i = 0; i < segments; ++i)
    {
        auto angle = (float) i / (float) segments * 2.0f * pi;
        sub.points.add({centerX + std::cos(angle) * outsetX,
                        centerY + std::sin(angle) * outsetY});
    }

    sub.closed = true;
    subPaths.add(std::move(sub));
    currentPoint = {centerX + radiusX, centerY};
}

void Path::append(const Path& other)
{
    for (const auto& sub: other.subPaths)
        subPaths.add(sub);

    if (!other.subPaths.empty())
        currentPoint = other.currentPoint;
}

Path Path::transformed(const AffineTransform& transform) const
{
    auto result = *this;

    for (auto& sub: result.subPaths)
        for (auto& point: sub.points)
            point = transform.apply(point);

    result.currentPoint = transform.apply(currentPoint);

    return result;
}

Path Path::scaled(float scaleX, float scaleY) const
{
    return transformed(AffineTransform::scaling(scaleX, scaleY));
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
