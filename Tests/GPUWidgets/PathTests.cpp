#include <eacp/GPUWidgets/GPUWidgets.h>

#include <NanoTest/NanoTest.h>

using namespace nano;
using namespace eacp;
using namespace eacp::GPUWidgets;

namespace
{
using Graphics::Point;

float triangleArea(const Point& a, const Point& b, const Point& c)
{
    return std::abs((b.x - a.x) * (c.y - a.y) - (b.y - a.y) * (c.x - a.x)) * 0.5f;
}

// Total area of a triangle list (every three consecutive points form a triangle).
float meshArea(const Vector<Point>& mesh)
{
    auto area = 0.0f;

    for (auto i = 0; i + 2 < mesh.size(); i += 3)
        area += triangleArea(mesh[i], mesh[i + 1], mesh[i + 2]);

    return area;
}

float polygonArea(const Vector<Point>& polygon)
{
    auto sum = 0.0f;
    auto count = polygon.size();

    for (auto i = 0; i < count; ++i)
    {
        const auto& a = polygon[i];
        const auto& b = polygon[(i + 1) % count];
        sum += a.x * b.y - b.x * a.y;
    }

    return std::abs(sum) * 0.5f;
}

// A concave (simple, non-self-intersecting) 5-point star, the same shape the
// Paths demo draws.
Vector<Point> starPoints(float centerX, float centerY, float outer, float inner)
{
    auto points = Vector<Point> {};

    for (auto i = 0; i < 10; ++i)
    {
        auto radius = (i % 2 == 0) ? outer : inner;
        auto angle = -pi * 0.5f + (float) i * pi / 5.0f;
        points.add({centerX + std::cos(angle) * radius,
                    centerY + std::sin(angle) * radius});
    }

    return points;
}
} // namespace

// A rectangle is one closed sub-path of four corners, and its bounds match the
// source rect. Pure geometry, no GPU device.
auto tPathRect = test("GPUWidgets/pathRectangle") = []
{
    auto path = Path {};
    path.addRect({10.0f, 20.0f, 100.0f, 40.0f});

    check(path.getSubPaths().size() == 1);
    check(path.getSubPaths()[0].points.size() == 4);
    check(path.getSubPaths()[0].closed);

    auto bounds = path.getBounds();
    check(std::abs(bounds.x - 10.0f) < 1e-4f);
    check(std::abs(bounds.y - 20.0f) < 1e-4f);
    check(std::abs(bounds.w - 100.0f) < 1e-4f);
    check(std::abs(bounds.h - 40.0f) < 1e-4f);
};

// Curves flatten to several line segments, so a cubic produces many more points
// than its two endpoints.
auto tPathFlatten = test("GPUWidgets/pathCurveFlattening") = []
{
    auto path = Path {};
    path.moveTo({0.0f, 0.0f});
    path.cubicTo(0.0f, 100.0f, 100.0f, 100.0f, 100.0f, 0.0f);

    const auto& sub = path.getSubPaths()[0];
    check(path.getSubPaths().size() == 1);
    check(sub.points.size() > 8);

    // The flattened curve stays within the convex hull of its control points.
    for (const auto& point: sub.points)
    {
        check(point.x >= -1e-3f && point.x <= 100.0f + 1e-3f);
        check(point.y >= -1e-3f && point.y <= 100.0f + 1e-3f);
    }
};

// A convex quad triangulates into exactly two triangles (six vertices) and the
// triangle list covers the same area as the quad.
auto tTessellateRect = test("GPUWidgets/tessellateRectangle") = []
{
    auto path = Path {};
    path.addRect({0.0f, 0.0f, 80.0f, 60.0f});

    auto mesh = tessellateFill(path);

    check(mesh.size() == 6);
    check(std::abs(meshArea(mesh) - 80.0f * 60.0f) < 1e-2f);
};

// The ear clipper handles reflex corners: a concave star triangulates into n - 2
// triangles and preserves the polygon's area.
auto tTessellateStar = test("GPUWidgets/tessellateConcaveStar") = []
{
    auto points = starPoints(200.0f, 200.0f, 120.0f, 50.0f);

    auto path = Path {};
    path.moveTo(points[0]);

    for (auto i = 1; i < points.size(); ++i)
        path.lineTo(points[i]);

    path.close();

    auto mesh = tessellateFill(path);

    // 10 vertices -> 8 triangles -> 24 points.
    check(mesh.size() == 24);
    check(std::abs(meshArea(mesh) - polygonArea(points)) < 1.0f);
};

// An empty path tessellates to nothing rather than misbehaving.
auto tTessellateEmpty = test("GPUWidgets/tessellateEmpty") = []
{
    auto path = Path {};
    check(tessellateFill(path).empty());
};

// A closed square strokes into a non-empty triangle list whose vertices all stay
// within the square inflated by half the stroke width. (Overlapping join triangles
// make a summed-area check unreliable, so this checks containment instead.)
auto tStroke = test("GPUWidgets/strokeClosedSquare") = []
{
    auto path = Path {};
    path.addRect({0.0f, 0.0f, 100.0f, 100.0f});

    auto width = 10.0f;
    auto half = width * 0.5f;
    auto mesh = tessellateStroke(path, width);

    check(!mesh.empty());
    check(mesh.size() % 3 == 0);

    for (const auto& point: mesh)
    {
        check(point.x >= -half - 1e-3f && point.x <= 100.0f + half + 1e-3f);
        check(point.y >= -half - 1e-3f && point.y <= 100.0f + half + 1e-3f);
    }
};

// A non-positive width strokes to nothing.
auto tStrokeZero = test("GPUWidgets/strokeZeroWidth") = []
{
    auto path = Path {};
    path.addRect({0.0f, 0.0f, 100.0f, 100.0f});

    check(tessellateStroke(path, 0.0f).empty());
};

// A linear gradient samples its endpoint colours, the midpoint between two stops,
// clamps outside the axis, and ignores the off-axis component.
auto tGradient = test("GPUWidgets/linearGradient") = []
{
    auto gradient =
        Graphics::LinearGradient {{0.0f, 0.0f},
                                  {100.0f, 0.0f},
                                  {{Graphics::Color {1.0f, 0.0f, 0.0f}, 0.0f},
                                   {Graphics::Color {0.0f, 0.0f, 1.0f}, 1.0f}}};

    auto start = colorAt(gradient, {0.0f, 0.0f});
    check(std::abs(start.r - 1.0f) < 1e-4f && std::abs(start.b - 0.0f) < 1e-4f);

    auto end = colorAt(gradient, {100.0f, 0.0f});
    check(std::abs(end.r - 0.0f) < 1e-4f && std::abs(end.b - 1.0f) < 1e-4f);

    auto mid = colorAt(gradient, {50.0f, 0.0f});
    check(std::abs(mid.r - 0.5f) < 1e-3f && std::abs(mid.b - 0.5f) < 1e-3f);

    auto before = colorAt(gradient, {-50.0f, 0.0f});
    check(std::abs(before.r - 1.0f) < 1e-4f);

    auto after = colorAt(gradient, {200.0f, 0.0f});
    check(std::abs(after.b - 1.0f) < 1e-4f);

    // Off-axis points project onto the axis: y is ignored for a horizontal axis.
    auto offAxis = colorAt(gradient, {50.0f, 999.0f});
    check(std::abs(offAxis.r - 0.5f) < 1e-3f);
};

// The middle stop of a three-stop gradient shows at the right place.
auto tGradientThreeStops = test("GPUWidgets/linearGradientThreeStops") = []
{
    auto gradient =
        Graphics::LinearGradient {{0.0f, 0.0f},
                                  {100.0f, 0.0f},
                                  {{Graphics::Color {1.0f, 0.0f, 0.0f}, 0.0f},
                                   {Graphics::Color {0.0f, 1.0f, 0.0f}, 0.5f},
                                   {Graphics::Color {0.0f, 0.0f, 1.0f}, 1.0f}}};

    auto middle = colorAt(gradient, {50.0f, 0.0f});
    check(std::abs(middle.g - 1.0f) < 1e-3f);
    check(std::abs(middle.r - 0.0f) < 1e-3f);
    check(std::abs(middle.b - 0.0f) < 1e-3f);
};

// The vertex-colour shader's layout is position (float2) + colour (float4), derived
// from the GradientVertex struct so it cannot drift from the upload type.
auto tVertexColorLayout = test("GPUWidgets/vertexColorShaderLayout") = []
{
    auto shader = VertexColorShader {};
    const auto& layout = shader.vertexLayout();

    check(layout.attributes.size() == 2);
    check(layout.attributes[0].format == GPU::VertexFormat::Float2);
    check(layout.attributes[0].offset == 0);
    check(layout.attributes[1].format == GPU::VertexFormat::Float4);
    check(layout.attributes[1].offset == (int) sizeof(Graphics::Point));
    check(layout.stride == (int) sizeof(GradientVertex));
};

// The fill shader's vertex layout is a single float2 position derived from the
// FillVertex struct, so it cannot drift from the upload type. Device-free.
auto tFillLayout = test("GPUWidgets/fillShaderLayout") = []
{
    auto shader = PathFillShader {};
    const auto& layout = shader.vertexLayout();

    check(layout.attributes.size() == 1);
    check(layout.attributes[0].format == GPU::VertexFormat::Float2);
    check(layout.attributes[0].offset == 0);
    check(layout.stride == (int) sizeof(FillVertex));
    check(layout.stride == (int) (sizeof(float) * 2));
};

// The shader cases that were here are in ShaderPipelineTests.cpp.

// The rect a turned rect is inside, which is what a scissor, a texture size or
// a damaged area is asked for -- a rotated rectangle not being one.
auto tTransformedBounds = test("GPUWidgets/aTurnedRectIsBoundedByItsCorners") = []
{
    auto square = Graphics::Rect {10.f, 10.f, 20.f, 20.f};

    auto moved = AffineTransform::translation(5.f, -5.f).apply(square);

    check(moved.x == 15.f && moved.y == 5.f);
    check(moved.w == 20.f && moved.h == 20.f, "a translation moves the bounds");

    auto turned =
        AffineTransform::rotationAbout(pi / 4.f, {20.f, 20.f}).apply(square);

    auto halfDiagonal = std::sqrt(200.f);

    check(std::abs(turned.w - halfDiagonal * 2.f) < 0.01f);
    check(std::abs(turned.x - (20.f - halfDiagonal)) < 0.01f,
          "and a quarter of a turn grows them to the diagonal");
};

namespace
{
bool near(const Point& a, const Point& b, float tolerance = 1e-3f)
{
    return std::abs(a.x - b.x) < tolerance && std::abs(a.y - b.y) < tolerance;
}

float ellipseRadius(const Point& point, const Point& centre, float rx, float ry)
{
    auto dx = (point.x - centre.x) / rx;
    auto dy = (point.y - centre.y) / ry;
    return std::sqrt(dx * dx + dy * dy);
}
} // namespace

// juce::Path's convention: zero is 12 o'clock and angles grow clockwise, so a
// quarter turn from 0 runs from the top of the circle to its right-hand side,
// through the upper-right quadrant.
auto tArcConvention = test("GPUWidgets/anArcFromZeroToAQuarterGoesTopToRight") = []
{
    auto path = Path {};
    path.addArc({0.0f, 0.0f, 100.0f, 100.0f}, 0.0f, pi * 0.5f, true);

    const auto& points = path.getSubPaths()[0].points;

    check(near(points.front(), {50.0f, 0.0f}), "starts at the top");
    check(near(points.back(), {100.0f, 50.0f}), "and ends at the right");

    for (const auto& point: points)
        check(point.x >= 50.0f - 1e-3f && point.y <= 50.0f + 1e-3f);
};

// Every flattened point of a quarter of an ellipse lies on that ellipse, to
// the path's flatness.
auto tArcOnEllipse = test("GPUWidgets/aQuarterArcStaysOnItsEllipse") = []
{
    auto path = Path {};
    path.addArc({10.0f, 20.0f, 200.0f, 100.0f}, pi * 0.5f, pi, true);

    const auto& points = path.getSubPaths()[0].points;

    check(points.size() > 4);
    check(near(points.front(), {210.0f, 70.0f}), "starts at 3 o'clock");
    check(near(points.back(), {110.0f, 120.0f}), "and ends at 6 o'clock");

    for (const auto& point: points)
        check(std::abs(ellipseRadius(point, {110.0f, 70.0f}, 100.0f, 50.0f) - 1.0f)
              < 1e-3f);
};

// A reversed pair of angles sweeps the other way round rather than wrapping.
auto tArcReversed = test("GPUWidgets/aReversedArcSweepsAnticlockwise") = []
{
    auto path = Path {};
    path.addArc({0.0f, 0.0f, 100.0f, 100.0f}, 0.0f, -pi * 0.5f, true);

    const auto& points = path.getSubPaths()[0].points;

    check(near(points.front(), {50.0f, 0.0f}));
    check(near(points.back(), {0.0f, 50.0f}));

    for (const auto& point: points)
        check(point.x <= 50.0f + 1e-3f && point.y <= 50.0f + 1e-3f);
};

// A whole turn comes back to where it began.
auto tArcFullTurn = test("GPUWidgets/aFullTurnArcClosesOnItsStart") = []
{
    auto path = Path {};
    path.addArc({0.0f, 0.0f, 100.0f, 100.0f}, 0.0f, 2.0f * pi, true);

    const auto& points = path.getSubPaths()[0].points;
    auto bounds = path.getBounds();

    check(near(points.front(), points.back()), "ends where it started");
    check(std::abs(bounds.w - 100.0f) < 0.05f && std::abs(bounds.h - 100.0f) < 0.05f,
          "and goes all the way round");
};

// Without startAsNewSubPath the arc joins the current sub-path with a straight
// line to its first point, as juce::Path does.
auto tArcContinues = test("GPUWidgets/anArcContinuesTheCurrentSubPath") = []
{
    auto path = Path {};
    path.moveTo({0.0f, 0.0f});
    path.addArc({0.0f, 0.0f, 100.0f, 100.0f}, 0.0f, pi * 0.5f);

    check(path.getSubPaths().size() == 1);

    const auto& points = path.getSubPaths()[0].points;

    check(near(points[0], {0.0f, 0.0f}));
    check(near(points[1], {50.0f, 0.0f}), "a line to the arc's start");
    check(near(points.back(), {100.0f, 50.0f}));

    path.addArc({0.0f, 0.0f, 100.0f, 100.0f}, 0.0f, pi * 0.5f, true);
    check(path.getSubPaths().size() == 2, "and a new one when asked");
};

// A rotated ellipse turns clockwise about its centre: a wide ellipse turned a
// quarter is a tall one, so its 12 o'clock point is where 3 o'clock was.
auto tCentredArcRotation = test("GPUWidgets/aCentredArcTurnsItsEllipse") = []
{
    auto path = Path {};
    path.addCentredArc(50.0f, 50.0f, 40.0f, 20.0f, pi * 0.5f, 0.0f, pi * 0.5f, true);

    const auto& points = path.getSubPaths()[0].points;

    check(near(points.front(), {70.0f, 50.0f}));
    check(near(points.back(), {50.0f, 90.0f}));
};

// A ring segment is one closed sub-path wholly between its two radii, and
// tessellates to the area of the annular sector it is.
auto tPieRing = test("GPUWidgets/aPieSegmentWithAHoleLiesBetweenItsRadii") = []
{
    auto path = Path {};
    path.addPieSegment({0.0f, 0.0f, 200.0f, 200.0f}, 0.0f, pi * 0.5f, 0.5f);

    check(path.getSubPaths().size() == 1);

    const auto& sub = path.getSubPaths()[0];
    check(sub.closed);

    for (const auto& point: sub.points)
    {
        auto radius = ellipseRadius(point, {100.0f, 100.0f}, 100.0f, 100.0f);
        check(radius > 0.5f - 1e-3f && radius < 1.0f + 1e-3f);
    }

    auto expected = (100.0f * 100.0f - 50.0f * 50.0f) * pi * 0.25f;
    check(std::abs(meshArea(tessellateFill(path)) - expected) < expected * 0.01f);
};

// With no hole it is a wedge back to the centre.
auto tPieWedge = test("GPUWidgets/aPieSegmentWithoutAHoleIsAWedge") = []
{
    auto path = Path {};
    path.addPieSegment({0.0f, 0.0f, 200.0f, 200.0f}, 0.0f, pi * 0.5f, 0.0f);

    const auto& sub = path.getSubPaths()[0];

    check(path.getSubPaths().size() == 1 && sub.closed);
    check(near(sub.points.front(), {100.0f, 0.0f}));
    check(near(sub.points.back(), {100.0f, 100.0f}), "closes through the centre");

    auto expected = 100.0f * 100.0f * pi * 0.25f;
    check(std::abs(meshArea(tessellateFill(path)) - expected) < expected * 0.01f);
};

// A full ring is two closed sub-paths, the hole wound against the outline.
auto tPieFullRing = test("GPUWidgets/aFullTurnPieSegmentIsARing") = []
{
    auto path = Path {};
    path.addPieSegment({0.0f, 0.0f, 200.0f, 200.0f}, 0.0f, 2.0f * pi, 0.5f);

    check(path.getSubPaths().size() == 2);
    check(path.getSubPaths()[0].closed && path.getSubPaths()[1].closed);

    auto signedArea = [](const Vector<Point>& polygon)
    {
        auto sum = 0.0f;

        for (auto i = 0; i < polygon.size(); ++i)
        {
            const auto& a = polygon[i];
            const auto& b = polygon[(i + 1) % polygon.size()];
            sum += a.x * b.y - b.x * a.y;
        }

        return sum * 0.5f;
    };

    auto outer = signedArea(path.getSubPaths()[0].points);
    auto inner = signedArea(path.getSubPaths()[1].points);

    check(outer * inner < 0.0f, "the hole winds the other way");
    check(std::abs(std::abs(inner) - 50.0f * 50.0f * pi)
          < 50.0f * 50.0f * pi * 0.01f);
};

// applyTransform is transformed in place.
auto tApplyTransform = test("GPUWidgets/applyTransformMatchesTransformed") = []
{
    auto path = Path {};
    path.addArc({0.0f, 0.0f, 100.0f, 100.0f}, 0.0f, pi, true);

    auto transform = AffineTransform::rotationAbout(0.3f, {10.0f, 20.0f});
    auto expected = path.transformed(transform);
    path.applyTransform(transform);

    const auto& got = path.getSubPaths()[0].points;
    const auto& want = expected.getSubPaths()[0].points;

    check(got.size() == want.size());

    for (auto i = 0; i < got.size(); ++i)
        check(near(got[i], want[i], 1e-6f));
};
