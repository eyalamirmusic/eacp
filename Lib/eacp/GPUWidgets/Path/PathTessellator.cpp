#include "PathTessellator.h"
#include <algorithm>

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

// Ear clipping proper, of a polygon already cleaned of duplicates and wound
// counter-clockwise. Appends 3 points per triangle and nothing else, so a caller
// can tell a polygon it consumed whole (n - 2 triangles) from one it gave up on.
void earClipSimple(const Vector<Point>& polygon, Vector<Point>& out)
{
    if (polygon.size() < 3)
        return;

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

void earClip(const Vector<Point>& source, Vector<Point>& out)
{
    auto polygon = cleanPolygon(source);

    // Normalise to counter-clockwise so a convex (ear) corner is a positive cross
    // product throughout.
    if (signedArea(polygon) < 0.0f)
        std::reverse(polygon.begin(), polygon.end());

    earClipSimple(polygon, out);
}

// Past this many points a contour is left to the coverage kernel, which reads
// segments in parallel on the GPU and does not care how many there are. Ear
// clipping does: it rescans the remaining vertices for every triangle it finds,
// so the work grows as the cube of the count on a contour full of reflex
// corners, and the crossing test below is quadratic on top of that.
constexpr int maxMeshPolygonPoints = 512;

// Whether the polygon's edges cross one another.
//
// Ear clipping cannot answer this and does not notice: it tests a candidate
// triangle against the other *vertices*, so a contour that crosses itself
// between them comes back fully consumed and quietly wrong. A five-pointed star
// written as five crossing edges -- which is how SVG documents write one -- would
// fill as a pentagon, and nothing about the result would say so.
bool edgesCross(const Point& a, const Point& b, const Point& c, const Point& d)
{
    auto side = [](float value)
    { return value > epsilon ? 1 : (value < -epsilon ? -1 : 0); };

    // Each segment strictly straddling the other's line. Touches are left alone:
    // they read as zero here, and a contour that merely grazes itself
    // triangulates well enough that refusing it would cost more than it saves.
    return side(cross(c, d, a)) * side(cross(c, d, b)) < 0
           && side(cross(a, b, c)) * side(cross(a, b, d)) < 0;
}

bool isSimplePolygon(const Vector<Point>& polygon)
{
    auto count = polygon.size();

    for (auto i = 0; i < count; ++i)
    {
        // From the edge after the next one, and stopping short of the edge
        // before this one at the wrap: adjacent edges share a point by
        // construction and would read as a crossing for ever.
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

// The outward normal of the edge from a to b, for a polygon wound so that
// signedArea is positive.
Point edgeNormal(const Point& a, const Point& b)
{
    auto dx = b.x - a.x;
    auto dy = b.y - a.y;
    auto length = std::sqrt(dx * dx + dy * dy);

    if (length < epsilon)
        return {};

    return {dy / length, -dx / length};
}

// How far past its edges a mitred corner may reach. A corner approaching a spike
// would otherwise grow a long spit of feather, which is more visible than the
// corner it exists to soften.
constexpr float maxMiterExtension = 4.0f;

// Every vertex moved `distance` along its own outward miter -- positive away
// from the interior, negative into it. Mitred rather than simply displaced along
// the normals, because that is what keeps each offset edge parallel to the one
// it came from, and therefore the band between them an even width round a
// corner.
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

        // The two edges double back on one another, so there is no bisector to
        // be had and the edge's own normal is the honest answer.
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

    // The outline ends up halfway between the two rings, which is where a mask
    // would have put its 50% coverage: the shape neither grows nor shrinks by
    // gaining an antialiased edge.
    auto half = std::max(0.0f, featherWidth) * 0.5f;
    auto inner = offsetRing(polygon, -half);
    auto outer = offsetRing(polygon, half);

    auto interior = Vector<Point> {};
    earClipSimple(inner, interior);

    // Ear clipping consumes a simple polygon whole -- n vertices become n - 2
    // triangles -- so anything less is geometry it could not read. Pulling the
    // ring inwards is its own test of that: a shape with a feature thinner than
    // the feather folds through itself, and the fill that came back would have a
    // bite out of it.
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
