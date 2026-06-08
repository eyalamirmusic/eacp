#include <eacp/GPUWidgets/GPUWidgets.h>

#include <NanoTest/NanoTest.h>

#include <cmath>
#include <string>

using namespace nano;
using namespace eacp;
using namespace eacp::GPUWidgets;

namespace
{
using Graphics::Point;

bool contains(const std::string& haystack, const std::string& needle)
{
    return haystack.find(needle) != std::string::npos;
}

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
    constexpr auto pi = 3.14159265358979323846f;
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

// The generated source carries the viewport + colour uniform block and routes the
// colour through a varying (uniforms bind to the vertex stage only). Backend-
// agnostic substring checks. Pure string generation.
auto tFillCodegen = test("GPUWidgets/fillShaderCodegen") = []
{
    auto shader = PathFillShader {};
    const auto& source = shader.source().source;

    check(contains(source, "struct Uniforms"));
    check(contains(source, "float2 u0")); // viewport
    check(contains(source, "float4 u1")); // colour
    check(contains(source, "v0")); // colour routed through a varying

    check(shader.source().vertexEntry == "vertexMain");
    check(shader.source().fragmentEntry == "fragmentMain");
};

// The real generated source compiles through the platform shader compiler and a
// pipeline builds from its layout. Self-skips on hosts without a GPU device
// (matches the GPU module's codegenCompiles test).
auto tFillCompiles = test("GPUWidgets/fillShaderCompiles") = []
{
    auto& device = GPU::Device::shared();

    if (!device.isValid())
        return;

    auto shader = PathFillShader {};

    auto library = device.makeShaderLibrary(shader.source());
    check(library.isValid());

    auto descriptor = GPU::RenderPipelineDescriptor {};
    descriptor.library = &library;
    descriptor.vertexLayout = shader.vertexLayout();

    auto pipeline = device.makeRenderPipeline(descriptor);
    check(pipeline.isValid());
};
