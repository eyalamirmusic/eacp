#include <eacp/GPUWidgets/GPUWidgets.h>

#include <NanoTest/NanoTest.h>

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

auto tTessellateRect = test("GPUWidgets/tessellateRectangle") = []
{
    auto path = Path {};
    path.addRect({0.0f, 0.0f, 80.0f, 60.0f});

    auto mesh = tessellateFill(path);

    check(mesh.size() == 6);
    check(std::abs(meshArea(mesh) - 80.0f * 60.0f) < 1e-2f);
};

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

auto tTessellateEmpty = test("GPUWidgets/tessellateEmpty") = []
{
    auto path = Path {};
    check(tessellateFill(path).empty());
};

// Overlapping join triangles make a summed-area check unreliable, so this checks
// containment within the square inflated by half the stroke width instead.
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

auto tStrokeZero = test("GPUWidgets/strokeZeroWidth") = []
{
    auto path = Path {};
    path.addRect({0.0f, 0.0f, 100.0f, 100.0f});

    check(tessellateStroke(path, 0.0f).empty());
};

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

    auto offAxis = colorAt(gradient, {50.0f, 999.0f});
    check(std::abs(offAxis.r - 0.5f) < 1e-3f);
};

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

auto tVertexColorCompiles = test("GPUWidgets/vertexColorShaderCompiles") = []
{
    auto& device = GPU::Device::shared();

    if (!device.isValid())
        return;

    auto shader = VertexColorShader {};

    auto library = device.makeShaderLibrary(shader.source());
    check(library.isValid());

    auto descriptor = GPU::RenderPipelineDescriptor {};
    descriptor.library = &library;
    descriptor.vertexLayout = shader.vertexLayout();

    auto pipeline = device.makeRenderPipeline(descriptor);
    check(pipeline.isValid());
};

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

auto tFillCodegen = test("GPUWidgets/fillShaderCodegen") = []
{
    auto shader = PathFillShader {};
    const auto& source = shader.source().source;

    check(contains(source, "struct Uniforms"));
    check(contains(source, "float2 u0")); // viewport
    check(contains(source, "float4 u1")); // colour
    check(contains(source, "return uniforms.u1;")); // read per-fragment
    check(!contains(source, "v0")); // no varying in between

    check(shader.source().vertexEntry == "vertexMain");
    check(shader.source().fragmentEntry == "fragmentMain");
};

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
