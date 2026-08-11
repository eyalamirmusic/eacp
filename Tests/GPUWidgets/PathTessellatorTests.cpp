#include <eacp/GPUWidgets/GPUWidgets.h>

#include <NanoTest/NanoTest.h>

#include <algorithm>
#include <cmath>

using namespace nano;
using namespace eacp;
using namespace eacp::GPUWidgets;

namespace
{
using Graphics::Point;

constexpr auto feather = 1.f;

// Points within this many feathers of an edge are inside the antialiasing ramp
// and are left unjudged.
constexpr auto margin = 1.5f;

Path rectangle()
{
    auto path = Path {};
    path.addRect({40.f, 30.f, 220.f, 160.f});
    return path;
}

Path circle()
{
    auto path = Path {};
    path.addEllipse({20.f, 20.f, 240.f, 240.f});
    return path;
}

// One deep reflex corner, so ear clipping cannot just take the first vertex.
Path arrow()
{
    auto path = Path {};
    path.moveTo({40.f, 40.f});
    path.lineTo({240.f, 120.f});
    path.lineTo({40.f, 200.f});
    path.lineTo({90.f, 120.f});
    path.close();
    return path;
}

Vector<Path> everyShape()
{
    auto shapes = Vector<Path> {};
    shapes.add(rectangle());
    shapes.add(circle());
    shapes.add(arrow());
    return shapes;
}

float cross(const Point& a, const Point& b, const Point& c)
{
    return (b.x - a.x) * (c.y - a.y) - (b.y - a.y) * (c.x - a.x);
}

// The triangles do not overlap, so the first hit is the answer.
float coverageAt(const Vector<MeshVertex>& mesh, const Point& at)
{
    for (auto index = 0; index + 2 < mesh.size(); index += 3)
    {
        const auto& a = mesh[index];
        const auto& b = mesh[index + 1];
        const auto& c = mesh[index + 2];

        auto total = cross(a.position, b.position, c.position);

        if (std::abs(total) < 1e-9f)
            continue;

        auto u = cross(b.position, c.position, at) / total;
        auto v = cross(c.position, a.position, at) / total;
        auto w = cross(a.position, b.position, at) / total;

        if (u < 0.f || v < 0.f || w < 0.f)
            continue;

        return u * a.coverage + v * b.coverage + w * c.coverage;
    }

    return 0.f;
}

float distanceToSegment(const Point& p, const Point& a, const Point& b)
{
    auto dx = b.x - a.x;
    auto dy = b.y - a.y;
    auto lengthSquared = dx * dx + dy * dy;

    auto t = lengthSquared > 0.f
                 ? std::clamp(((p.x - a.x) * dx + (p.y - a.y) * dy) / lengthSquared,
                              0.f,
                              1.f)
                 : 0.f;

    auto nearestX = a.x + dx * t;
    auto nearestY = a.y + dy * t;

    return std::sqrt((p.x - nearestX) * (p.x - nearestX)
                     + (p.y - nearestY) * (p.y - nearestY));
}

float distanceToOutline(const Point& p, const Path& path)
{
    auto best = std::numeric_limits<float>::max();

    for (const auto& sub: path.getSubPaths())
    {
        auto count = sub.points.size();

        for (auto i = 0; i < count; ++i)
            best = std::min(
                best,
                distanceToSegment(p, sub.points[i], sub.points[(i + 1) % count]));
    }

    return best;
}

bool isInside(const Point& p, const Path& path)
{
    auto crossings = 0;

    for (const auto& sub: path.getSubPaths())
    {
        auto count = sub.points.size();

        for (auto i = 0; i < count; ++i)
        {
            const auto& a = sub.points[i];
            const auto& b = sub.points[(i + 1) % count];

            if ((a.y > p.y) != (b.y > p.y)
                && p.x < a.x + (p.y - a.y) / (b.y - a.y) * (b.x - a.x))
                ++crossings;
        }
    }

    return crossings % 2 == 1;
}

float inkedArea(const Vector<MeshVertex>& mesh)
{
    auto total = 0.f;

    for (auto index = 0; index + 2 < mesh.size(); index += 3)
    {
        const auto& a = mesh[index];
        const auto& b = mesh[index + 1];
        const auto& c = mesh[index + 2];

        auto area = std::abs(cross(a.position, b.position, c.position)) * 0.5f;
        total += area * (a.coverage + b.coverage + c.coverage) / 3.f;
    }

    return total;
}

float polygonArea(const Path& path)
{
    auto total = 0.f;

    for (const auto& sub: path.getSubPaths())
    {
        auto sum = 0.f;
        auto count = sub.points.size();

        for (auto i = 0; i < count; ++i)
        {
            const auto& a = sub.points[i];
            const auto& b = sub.points[(i + 1) % count];
            sum += a.x * b.y - b.x * a.y;
        }

        total += std::abs(sum) * 0.5f;
    }

    return total;
}

Graphics::Rect grownBounds(const Path& path, float by)
{
    auto bounds = path.getBounds();
    return {bounds.x - by, bounds.y - by, bounds.w + by * 2.f, bounds.h + by * 2.f};
}
} // namespace

// Sampled beyond the bounds as well as inside them: a mesh that filled the whole
// bounding box would pass every interior check there is.
auto tCoversTheShape = test("PathTessellator/meshCoversExactlyTheShape") = []
{
    for (const auto& shape: everyShape())
    {
        auto mesh = tessellateAntialiasedFill(shape, feather);
        check(!mesh.empty());

        auto area = grownBounds(shape, feather * 4.f);
        auto missing = 0;
        auto extra = 0;

        for (auto y = 0; y < 120; ++y)
        {
            for (auto x = 0; x < 120; ++x)
            {
                auto at = Point {area.x + area.w * ((float) x + 0.5f) / 120.f,
                                 area.y + area.h * ((float) y + 0.5f) / 120.f};

                auto distance = distanceToOutline(at, shape);

                if (distance < feather * margin)
                    continue;

                auto covered = coverageAt(mesh, at);

                if (isInside(at, shape))
                {
                    if (covered < 0.99f)
                        ++missing;
                }
                else if (covered > 0.01f)
                {
                    ++extra;
                }
            }
        }

        check(missing == 0, std::to_string(missing) + " points inside, unfilled");
        check(extra == 0, std::to_string(extra) + " points outside, filled");
    }
};

// A feather added to the outline rather than centred on it would make every
// meshed shape half a pixel fatter than the same shape drawn from a mask. The
// slack is the mitred corners, at most a feather squared of area.
auto tInkMatchesTheArea = test("PathTessellator/featherIsCentredOnTheOutline") = []
{
    for (const auto& shape: everyShape())
    {
        auto mesh = tessellateAntialiasedFill(shape, feather);
        auto difference = std::abs(inkedArea(mesh) - polygonArea(shape));

        check(difference < feather * feather * 4.f,
              "inked area differs by " + std::to_string(difference));
    }
};

// A mesh that overlapped itself looks right in every opaque test and wrong the
// moment a document sets an opacity.
auto tDoesNotOverlapItself = test("PathTessellator/trianglesDoNotOverlap") = []
{
    for (const auto& shape: everyShape())
    {
        auto mesh = tessellateAntialiasedFill(shape, feather);
        auto area = grownBounds(shape, feather * 4.f);
        auto overlapping = 0;

        for (auto y = 0; y < 90; ++y)
        {
            for (auto x = 0; x < 90; ++x)
            {
                auto at = Point {area.x + area.w * ((float) x + 0.5f) / 90.f,
                                 area.y + area.h * ((float) y + 0.5f) / 90.f};

                auto hits = 0;

                for (auto index = 0; index + 2 < mesh.size(); index += 3)
                {
                    auto u = cross(
                        mesh[index + 1].position, mesh[index + 2].position, at);
                    auto v =
                        cross(mesh[index + 2].position, mesh[index].position, at);
                    auto w =
                        cross(mesh[index].position, mesh[index + 1].position, at);

                    // Strictly inside, so a point on a shared edge is not
                    // counted twice.
                    if ((u > 0.f && v > 0.f && w > 0.f)
                        || (u < 0.f && v < 0.f && w < 0.f))
                        ++hits;
                }

                if (hits > 1)
                    ++overlapping;
            }
        }

        check(overlapping == 0,
              std::to_string(overlapping)
                  + " points covered by more than one "
                    "triangle");
    }
};

// The tessellator has no fill rule to tell a hole from a second blob with.
auto tRefusesTwoContours = test("PathTessellator/refusesMoreThanOneContour") = []
{
    auto donut = Path {};
    donut.addEllipse({20.f, 20.f, 200.f, 200.f});
    donut.addEllipse({70.f, 70.f, 100.f, 100.f});

    check(tessellateAntialiasedFill(donut, feather).empty());

    auto two = Path {};
    two.addRect({0.f, 0.f, 50.f, 50.f});
    two.addRect({100.f, 0.f, 50.f, 50.f});

    check(tessellateAntialiasedFill(two, feather).empty());
};

// Ear clipping consumes a self-crossing contour without complaint, so an SVG
// five-pointed star would fill as a pentagon with nothing to say it had.
auto tRefusesSelfCrossing = test("PathTessellator/refusesACrossingContour") = []
{
    auto star = Path {};

    for (auto i = 0; i < 5; ++i)
    {
        // Every second point of a pentagon: five vertices, five crossings.
        auto angle = (float) (i * 2) / 5.f * 2.f * 3.14159265358979323846f;
        auto at =
            Point {120.f + std::cos(angle) * 100.f, 120.f + std::sin(angle) * 100.f};

        if (i == 0)
            star.moveTo(at);
        else
            star.lineTo(at);
    }

    star.close();

    check(tessellateAntialiasedFill(star, feather).empty());

    auto bowtie = Path {};
    bowtie.moveTo({0.f, 0.f});
    bowtie.lineTo({100.f, 100.f});
    bowtie.lineTo({100.f, 0.f});
    bowtie.lineTo({0.f, 100.f});
    bowtie.close();

    check(tessellateAntialiasedFill(bowtie, feather).empty());
};

auto tRefusesDegenerate = test("PathTessellator/refusesWhatIsNotAPolygon") = []
{
    check(tessellateAntialiasedFill(Path {}, feather).empty());

    auto line = Path {};
    line.moveTo({0.f, 0.f});
    line.lineTo({100.f, 0.f});

    check(tessellateAntialiasedFill(line, feather).empty());
};

// A ring pulled inwards further than the shape is thick folds through itself,
// and the fill that came back would have a bite out of it.
auto tRefusesThinnerThanTheFeather =
    test("PathTessellator/refusesAShapeThinnerThanItsFeather") = []
{
    auto sliver = Path {};
    sliver.addRect({0.f, 0.f, 200.f, 0.2f});

    auto mesh = tessellateAntialiasedFill(sliver, feather);

    check(mesh.empty() || inkedArea(mesh) < 200.f * feather);
};

// Ear clipping costs more than linearly, while the kernel it replaces reads
// segments in parallel -- so past a point the mesh is not the cheaper answer.
auto tRefusesTooManyPoints =
    test("PathTessellator/refusesAnUnreasonableContour") = []
{
    auto many = Path {};
    many.moveTo({120.f, 20.f});

    for (auto i = 1; i < 1200; ++i)
    {
        auto angle = (float) i / 1200.f * 2.f * 3.14159265358979323846f;
        many.lineTo(
            {120.f + std::sin(angle) * 100.f, 120.f - std::cos(angle) * 100.f});
    }

    many.close();

    check(many.getSubPaths()[0].points.size() > 512);
    check(tessellateAntialiasedFill(many, feather).empty());
};
