#include "PathTessellator.h"
#include <algorithm>

#include "../Common.h"

namespace eacp::GPUWidgets
{
namespace
{
using Graphics::Point;

constexpr int joinSegments = 12;

// Twice the signed area of abc: positive when it winds counter-clockwise in a
// y-up sense.
float cross(const Point& a, const Point& b, const Point& c)
{
    return (b.x - a.x) * (c.y - a.y) - (b.y - a.y) * (c.x - a.x);
}

// Twice the area; the sign gives the winding order.
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

// Drops duplicate and wrap-around points, leaving no zero-length edges.
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

// Takes a cleaned, counter-clockwise polygon. Appends exactly 3 points per
// triangle, so the count tells a whole triangulation from an abandoned one.
void earClipSimple(const Vector<Point>& polygon, Vector<Point>& out)
{
    if (polygon.size() < 3)
        return;

    auto remaining = Vector<int> {};

    for (auto i = 0; i < polygon.size(); ++i)
        remaining.add(i);

    // One sweep finds at least one ear, so n sweeps cannot stall.
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

void earClip(const Vector<Point>& source, Vector<Point>& out)
{
    auto polygon = cleanPolygon(source);

    // Counter-clockwise makes an ear corner a positive cross product.
    if (signedArea(polygon) < 0.0f)
        std::reverse(polygon.begin(), polygon.end());

    earClipSimple(polygon, out);
}

// Ear clipping is cubic and the crossing test quadratic; past this the coverage
// kernel is the cheaper route.
constexpr int maxMeshPolygonPoints = 512;

// Ear clipping tests candidate triangles against vertices, not edges, so a
// self-crossing contour is consumed whole and filled as the wrong shape.
bool edgesCross(const Point& a, const Point& b, const Point& c, const Point& d)
{
    auto side = [](float value)
    { return value > epsilon ? 1 : (value < -epsilon ? -1 : 0); };

    // Strict straddle only: a contour that merely grazes itself is accepted.
    return side(cross(c, d, a)) * side(cross(c, d, b)) < 0
           && side(cross(a, b, c)) * side(cross(a, b, d)) < 0;
}

bool isSimplePolygon(const Vector<Point>& polygon)
{
    auto count = polygon.size();

    for (auto i = 0; i < count; ++i)
    {
        // Skips adjacent edges, which share a point by construction.
        for (auto j = i + 2; j < count - (i == 0 ? 1 : 0); ++j)
        {
            if (edgesCross(polygon[i],
                           polygon[(i + 1) % count],
                           polygon[j],
                           polygon[(j + 1) % count]))
                return false;
        }
    }

    return true;
}

// Outward only for a polygon whose signedArea is positive.
Point edgeNormal(const Point& a, const Point& b)
{
    auto dx = b.x - a.x;
    auto dy = b.y - a.y;
    auto length = std::sqrt(dx * dx + dy * dy);

    if (length < epsilon)
        return {};

    return {dy / length, -dx / length};
}

// Bounds the feather spit a near-spike corner would otherwise grow.
constexpr float maxMiterExtension = 4.0f;

// Positive distance moves outward, negative into the interior. Mitred, so the
// offset edges stay parallel and the band keeps an even width round a corner.
Vector<Point> offsetRing(const Vector<Point>& polygon, float distance)
{
    auto count = polygon.size();
    auto ring = Vector<Point> {};
    ring.reserve(count);

    for (auto i = 0; i < count; ++i)
    {
        auto before = edgeNormal(polygon[(i + count - 1) % count], polygon[i]);
        auto after = edgeNormal(polygon[i], polygon[(i + 1) % count]);

        auto sumX = before.x + after.x;
        auto sumY = before.y + after.y;
        auto length = std::sqrt(sumX * sumX + sumY * sumY);

        // The edges double back, leaving no bisector; use the edge's normal.
        auto bisector =
            length < epsilon ? after : Point {sumX / length, sumY / length};

        auto cosHalfAngle = bisector.x * after.x + bisector.y * after.y;
        auto extension = cosHalfAngle > 1.0f / maxMiterExtension
                             ? 1.0f / cosHalfAngle
                             : maxMiterExtension;

        ring.add({polygon[i].x + bisector.x * distance * extension,
                  polygon[i].y + bisector.y * distance * extension});
    }

    return ring;
}

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

// Fills the wedge left outside a turn; hidden inside the quad on straight runs.
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

    // Round joins inside, round caps at the ends of an open sub-path.
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

Vector<MeshVertex> tessellateAntialiasedFill(const Path& path, float featherWidth)
{
    const auto& subPaths = path.getSubPaths();

    if (subPaths.size() != 1)
        return {};

    auto polygon = cleanPolygon(subPaths[0].points);

    if (polygon.size() < 3 || polygon.size() > maxMeshPolygonPoints)
        return {};

    if (!isSimplePolygon(polygon))
        return {};

    if (signedArea(polygon) < 0.0f)
        std::reverse(polygon.begin(), polygon.end());

    // The outline lands halfway between the rings, so the antialiased edge
    // neither grows nor shrinks the shape.
    auto half = std::max(0.0f, featherWidth) * 0.5f;
    auto inner = offsetRing(polygon, -half);
    auto outer = offsetRing(polygon, half);

    auto interior = Vector<Point> {};
    earClipSimple(inner, interior);

    // Fewer than n - 2 triangles means the inward ring self-folded, which a
    // feature thinner than the feather does.
    if (interior.size() != (inner.size() - 2) * 3)
        return {};

    auto mesh = Vector<MeshVertex> {};
    mesh.reserve(interior.size() + inner.size() * 6);

    for (const auto& point: interior)
        mesh.add({point, 1.0f});

    for (auto i = 0; i < inner.size(); ++i)
    {
        auto next = (i + 1) % inner.size();

        mesh.add({inner[i], 1.0f});
        mesh.add({outer[i], 0.0f});
        mesh.add({outer[next], 0.0f});

        mesh.add({inner[i], 1.0f});
        mesh.add({outer[next], 0.0f});
        mesh.add({inner[next], 1.0f});
    }

    return mesh;
}
} // namespace eacp::GPUWidgets
