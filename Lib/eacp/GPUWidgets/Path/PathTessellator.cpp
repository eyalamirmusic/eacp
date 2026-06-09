#include "PathTessellator.h"

#include <algorithm>
#include <cmath>

#include "../Common.h"

namespace eacp::GPUWidgets
{
namespace
{
using Graphics::Point;


// Round joins / caps are discs of this many triangles. Modest is plenty at
// stroke-width scale.
constexpr int joinSegments = 12;

// Twice the signed area of triangle abc. Positive when abc winds
// counter-clockwise (in a y-up sense), negative when clockwise.
float cross(const Point& a, const Point& b, const Point& c)
{
    return (b.x - a.x) * (c.y - a.y) - (b.y - a.y) * (c.x - a.x);
}

// Twice the signed area of the polygon; its sign gives the winding order.
float signedArea(const Vector<Point>& polygon)
{
    auto sum = 0.0f;
    auto count = polygon.size();

    for (auto i = 0; i < count; ++i)
    {
        const auto& a = polygon[i];
        const auto& b = polygon[(i + 1) % count];
        sum += a.x * b.y - b.x * a.y;
    }

    return sum;
}

bool pointInTriangle(const Point& p, const Point& a, const Point& b, const Point& c)
{
    auto d1 = cross(a, b, p);
    auto d2 = cross(b, c, p);
    auto d3 = cross(c, a, p);

    auto hasNegative = d1 < 0.0f || d2 < 0.0f || d3 < 0.0f;
    auto hasPositive = d1 > 0.0f || d2 > 0.0f || d3 > 0.0f;

    return !(hasNegative && hasPositive);
}

// Drops consecutive duplicate points and a trailing repeat of the first point, so
// the polygon ear clipping sees has no zero-length edges to stall on.
Vector<Point> cleanPolygon(const Vector<Point>& input)
{
    auto coincident = [](const Point& a, const Point& b)
    { return std::abs(a.x - b.x) < epsilon && std::abs(a.y - b.y) < epsilon; };

    auto polygon = Vector<Point> {};

    for (auto i = 0; i < input.size(); ++i)
    {
        if (polygon.empty() || !coincident(polygon.back(), input[i]))
            polygon.add(input[i]);
    }

    while (polygon.size() > 1 && coincident(polygon.front(), polygon.back()))
        polygon.erase(polygon.end() - 1);

    return polygon;
}

void earClip(const Vector<Point>& source, Vector<Point>& out)
{
    auto polygon = cleanPolygon(source);

    if (polygon.size() < 3)
        return;

    // Normalise to counter-clockwise so a convex (ear) corner is a positive cross
    // product throughout.
    if (signedArea(polygon) < 0.0f)
        std::reverse(polygon.begin(), polygon.end());

    auto remaining = Vector<int> {};

    for (auto i = 0; i < polygon.size(); ++i)
        remaining.add(i);

    // A simple polygon of n vertices triangulates into n - 2 triangles, each
    // found within one sweep, so n sweeps is a safe ceiling against a stall on
    // degenerate input.
    auto sweepLimit = polygon.size();

    while (remaining.size() > 3 && sweepLimit-- > 0)
    {
        auto clippedAnEar = false;
        auto count = remaining.size();

        for (auto i = 0; i < count; ++i)
        {
            auto previous = remaining[(i + count - 1) % count];
            auto current = remaining[i];
            auto next = remaining[(i + 1) % count];

            const auto& a = polygon[previous];
            const auto& b = polygon[current];
            const auto& c = polygon[next];

            if (cross(a, b, c) <= 0.0f)
                continue; // reflex or collinear: not an ear

            auto enclosesVertex = false;

            for (auto j = 0; j < count; ++j)
            {
                auto index = remaining[j];

                if (index == previous || index == current || index == next)
                    continue;

                if (pointInTriangle(polygon[index], a, b, c))
                {
                    enclosesVertex = true;
                    break;
                }
            }

            if (enclosesVertex)
                continue;

            out.add(a);
            out.add(b);
            out.add(c);

            remaining.erase(remaining.begin() + i);
            clippedAnEar = true;
            break;
        }

        if (!clippedAnEar)
            return; // degenerate / self-intersecting: stop rather than spin
    }

    if (remaining.size() == 3)
    {
        out.add(polygon[remaining[0]]);
        out.add(polygon[remaining[1]]);
        out.add(polygon[remaining[2]]);
    }
}

// One segment as a quad offset perpendicular by half the stroke width on each
// side, emitted as two triangles.
void addSegment(Vector<Point>& out, const Point& a, const Point& b, float half)
{
    auto dx = b.x - a.x;
    auto dy = b.y - a.y;
    auto length = std::sqrt(dx * dx + dy * dy);

    if (length < epsilon)
        return;

    auto normalX = -dy / length * half;
    auto normalY = dx / length * half;

    auto a0 = Point {a.x + normalX, a.y + normalY};
    auto a1 = Point {a.x - normalX, a.y - normalY};
    auto b0 = Point {b.x + normalX, b.y + normalY};
    auto b1 = Point {b.x - normalX, b.y - normalY};

    out.add(a0);
    out.add(a1);
    out.add(b1);
    out.add(a0);
    out.add(b1);
    out.add(b0);
}

// A filled disc as a triangle fan, used for round joins and round caps. Its
// outer half fills the wedge gap on the outside of a turn; on straight runs it
// sits inside the segment quad and shows nothing.
void addDisc(Vector<Point>& out, const Point& center, float radius)
{
    for (auto i = 0; i < joinSegments; ++i)
    {
        auto angle0 = (float) i / (float) joinSegments * 2.0f * pi;
        auto angle1 = (float) (i + 1) / (float) joinSegments * 2.0f * pi;

        out.add(center);
        out.add({center.x + std::cos(angle0) * radius,
                 center.y + std::sin(angle0) * radius});
        out.add({center.x + std::cos(angle1) * radius,
                 center.y + std::sin(angle1) * radius});
    }
}

void strokeSubPath(const Vector<Point>& source,
                   bool closed,
                   float half,
                   Vector<Point>& out)
{
    auto points = cleanPolygon(source);
    auto count = points.size();

    if (count < 2)
        return;

    auto segments = closed ? count : count - 1;

    for (auto i = 0; i < segments; ++i)
        addSegment(out, points[i], points[(i + 1) % count], half);

    // A disc at every vertex: a round join at interior vertices, a round cap at
    // the ends of an open sub-path.
    for (auto i = 0; i < count; ++i)
        addDisc(out, points[i], half);
}
} // namespace

Vector<Graphics::Point> tessellateFill(const Path& path)
{
    auto triangles = Vector<Graphics::Point> {};

    for (const auto& sub: path.getSubPaths())
        earClip(sub.points, triangles);

    return triangles;
}

Vector<Graphics::Point> tessellateStroke(const Path& path, float width)
{
    auto triangles = Vector<Graphics::Point> {};

    if (width <= 0.0f)
        return triangles;

    auto half = width * 0.5f;

    for (const auto& sub: path.getSubPaths())
        strokeSubPath(sub.points, sub.closed, half, triangles);

    return triangles;
}
} // namespace eacp::GPUWidgets
