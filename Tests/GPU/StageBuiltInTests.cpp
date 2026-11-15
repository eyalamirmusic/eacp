#include "Common.h"

// The render-stage built-ins of the shader graph: fragmentCoordinate(),
// vertexId() and instanceId(). Each one is drawn into an R32Float target and
// read back, so what is pinned is the number a shader gets rather than the
// spelling the emitter chose for it - that half is in ShaderCodegenTests.
//
// Runs on every backend, and self-skips without a GPU device.

using namespace nano;
using namespace eacp;
using namespace eacp::GPU;

namespace StageBuiltInTests
{
namespace
{
constexpr auto targetSize = 4;
constexpr auto pixelCount = targetSize * targetSize;
constexpr auto quadDepth = 0.25f;

struct CornerVertex
{
    float position[2];
};
} // namespace
} // namespace StageBuiltInTests

EACP_SHADER_VALUE(StageBuiltInTests::CornerVertex, Float2)

namespace StageBuiltInTests
{
namespace
{
constexpr CornerVertex fullQuad[] = {
    {{-1.f, -1.f}},
    {{1.f, -1.f}},
    {{-1.f, 1.f}},
    {{1.f, -1.f}},
    {{1.f, 1.f}},
    {{-1.f, 1.f}},
};

// The left half and then the right, so vertices 0..5 cover one and 6..11 the
// other.
constexpr CornerVertex twoHalves[] = {
    {{-1.f, -1.f}},
    {{0.f, -1.f}},
    {{-1.f, 1.f}},
    {{0.f, -1.f}},
    {{0.f, 1.f}},
    {{-1.f, 1.f}},
    {{0.f, -1.f}},
    {{1.f, -1.f}},
    {{0.f, 1.f}},
    {{1.f, -1.f}},
    {{1.f, 1.f}},
    {{0.f, 1.f}},
};

// One column of the target, which each instance moves along by its id.
constexpr CornerVertex firstColumn[] = {
    {{-1.f, -1.f}},
    {{-0.5f, -1.f}},
    {{-1.f, 1.f}},
    {{-0.5f, -1.f}},
    {{-0.5f, 1.f}},
    {{-1.f, 1.f}},
};

// x + 10 y, so a pixel's value says both where it is and which way y runs.
struct PixelPositionShader final : ShaderProgram
{
    PixelPositionShader() { compile(); }

    void define() override
    {
        auto position = vertexInput(&CornerVertex::position);

        setPosition(float4(position, 0.f, 1.f));

        auto at = fragmentCoordinate();

        setFragment(float4(at.x() + 10.f * at.y(), 0.f, 0.f, 1.f));
    }

    EACP_SHADER()
};

// z + 10 w, drawn with a clip w of 2: the depth after the divide, and 1 / w.
struct DepthAndWShader final : ShaderProgram
{
    DepthAndWShader() { compile(); }

    void define() override
    {
        auto position = vertexInput(&CornerVertex::position);

        setPosition(float4(position * 2.f, quadDepth * 2.f, 2.f));

        auto at = fragmentCoordinate();

        setFragment(float4(at.z() + 10.f * at.w(), 0.f, 0.f, 1.f));
    }

    EACP_SHADER()
};

struct VertexIdShader final : ShaderProgram
{
    VertexIdShader() { compile(); }

    void define() override
    {
        auto position = vertexInput(&CornerVertex::position);

        setPosition(float4(position, 0.f, 1.f));

        auto half = toFloat(varying(vertexId() / 6u));

        setFragment(float4(half, 0.f, 0.f, 1.f));
    }

    EACP_SHADER()
};

struct InstanceIdShader final : ShaderProgram
{
    InstanceIdShader() { compile(); }

    void define() override
    {
        auto position = vertexInput(&CornerVertex::position);
        auto instance = instanceId();
        auto offset = toFloat(instance) * 0.5f;

        setPosition(float4(position.x() + offset, position.y(), 0.f, 1.f));
        setFragment(float4(toFloat(varying(instance)), 0.f, 0.f, 1.f));
    }

    EACP_SHADER()
};

TextureDescriptor describeFloatTarget()
{
    auto descriptor = TextureDescriptor {};
    descriptor.width = targetSize;
    descriptor.height = targetSize;
    descriptor.format = TextureFormat::R32Float;
    descriptor.renderTarget = true;
    return descriptor;
}

template <typename Program>
void prepareInto(Program& program, const CornerVertex* vertices, int count)
{
    program.setVertices(vertices, count);
    program.prepare(1,
                    false,
                    PrimitiveTopology::Triangles,
                    BlendMode::None,
                    pixelFormatFor(TextureFormat::R32Float));
}

struct FrameRunner final : GPUView
{
    FrameRunner() { setSampleCount(1); }

    void render(Frame& frame) override
    {
        draw(frame);
        frame.beginPass({Graphics::Color::black()});
    }

    std::function<void(Frame&)> draw = [](Frame&) {};
};

// One pass into a float target cleared to -1, read back before the frame ends.
Array<float, pixelCount> drawAndRead(std::function<void(RenderPass&)> draw)
{
    auto target = Device::shared().makeTexture(describeFloatTarget());
    auto values = Array<float, pixelCount> {};

    auto view = FrameRunner {};
    view.draw = [&](Frame& frame)
    {
        {
            auto descriptor = RenderPassDescriptor {};
            descriptor.clearColor = {-1.f, 0.f, 0.f, 1.f};

            auto pass = frame.beginPass(target, descriptor);
            draw(pass);
        }

        frame.flush();
        target.read(values.data());
    };

    view.setBounds({0.f, 0.f, 4.f, 4.f});
    check(view.renderToImage(1.f).isValid());
    return values;
}

bool near(float value, float expected)
{
    return std::abs(value - expected) < 0.002f;
}

bool canTest()
{
    return Device::shared().isValid();
}
} // namespace

auto tFragmentCoordinateIsTopLeftPixels =
    test("StageBuiltIn/fragmentCoordinateCountsPixelsFromTheTopLeft") = []
{
    if (!canTest())
        return;

    auto program = PixelPositionShader {};
    prepareInto(program, fullQuad, 6);

    const auto values = drawAndRead([&](RenderPass& pass) { pass.draw(program); });

    for (auto y = 0; y < targetSize; ++y)
        for (auto x = 0; x < targetSize; ++x)
            check(near(values[y * targetSize + x], (x + 0.5f) + 10.f * (y + 0.5f)));
};

auto tFragmentCoordinateCarriesDepthAndW =
    test("StageBuiltIn/fragmentCoordinateCarriesDepthAndOneOverW") = []
{
    if (!canTest())
        return;

    auto program = DepthAndWShader {};
    prepareInto(program, fullQuad, 6);

    const auto values = drawAndRead([&](RenderPass& pass) { pass.draw(program); });

    for (auto value: values)
        check(near(value, quadDepth + 10.f * 0.5f));
};

auto tVertexIdCountsTheDrawsVertices =
    test("StageBuiltIn/vertexIdCountsTheVerticesOfTheDraw") = []
{
    if (!canTest())
        return;

    auto program = VertexIdShader {};
    prepareInto(program, twoHalves, 12);

    const auto values = drawAndRead([&](RenderPass& pass) { pass.draw(program); });

    for (auto i = 0; i < pixelCount; ++i)
        check(near(values[i], i % targetSize < targetSize / 2 ? 0.f : 1.f));
};

auto tInstanceIdCountsTheInstances =
    test("StageBuiltIn/instanceIdCountsTheInstancesOfTheDraw") = []
{
    if (!canTest())
        return;

    auto program = InstanceIdShader {};
    prepareInto(program, firstColumn, 6);

    const auto values =
        drawAndRead([&](RenderPass& pass) { pass.drawInstanced(program, 4); });

    for (auto i = 0; i < pixelCount; ++i)
        check(near(values[i], (float) (i % targetSize)));
};
} // namespace StageBuiltInTests
