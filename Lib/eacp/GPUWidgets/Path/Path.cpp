#include "Path.h"
#include <algorithm>

#include "../Common.h"

namespace eacp::GPUWidgets
{
namespace
{
// A ceiling on subdivision, so a degenerate control polygon or an absurd
// flatness cannot turn one curve into an unbounded point list.
constexpr int maxCurveSegments = 512;

Graphics::Point lerp(const Graphics::Point& a, const Graphics::Point& b, float t)
{
    return {a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t};
}

float length(const Graphics::Point& point)
{
    return std::sqrt(point.x * point.x + point.y * point.y);
}

// The second difference of three control points: what bounds how far a Bezier
// bows away from the chord across them.
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

// A Bezier split into n uniform pieces strays from its polyline by at most
// d / (8 n^2), where d is the largest second difference of its control points.
// Turned around, that is the count which holds the error to flatness.
int Path::segmentsForCurve(float difference) const
{
    auto needed = std::ceil(std::sqrt(difference / (8.0f * flatness)));
    return std::clamp((int) needed, 1, maxCurveSegments);
}

// The same question for an arc, where the deviation is the sagitta of one
// segment's chord: r (1 - cos(halfAngle)). Solving it for the angle gives the
// widest segment allowed, and the sweep divided by that gives the count.
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

    // A cubic bows about either of its two second differences, so the tighter
    // of the two decides the count.
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

    // The flatter axis bows least, so the longer one sets the count for the
    // whole sweep - subdividing an ellipse per quadrant would put a seam where
    // the counts changed.
    auto segments = segmentsForArc(std::max(radiusX, radiusY), 2.0f * pi);

    // Vertices placed straight on the ellipse make a polygon wholly inside it,
    // small by the full sagitta everywhere between them. Pushed out by half of
    // it the polygon straddles the real curve instead: the same error, but
    // signed both ways and averaging to nothing, which is what keeps a filled
    // circle the size it was asked for rather than reliably a little under.
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
