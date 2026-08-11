#include "Common.h"

// eacp states the winding convention in clip space: counter-clockwise is front,
// as glTF has it. Regression: D3D12 was set FrontCounterClockwise = FALSE and
// culled the opposite face on Windows.

using namespace nano;
using namespace eacp;
using namespace eacp::GPU;

namespace
{
constexpr auto viewWidth = 8;
constexpr auto viewHeight = 4;

struct QuadVertex
{
    float position[2];
};
} // namespace

EACP_SHADER_VALUE(QuadVertex, Float2)

namespace
{
// The left half, wound counter-clockwise in clip space (the space setPosition
// writes, y up), so it is the front face.
constexpr QuadVertex frontQuad[] = {
    {{-1.f, -1.f}},
    {{0.f, -1.f}},
    {{-1.f, 1.f}},
    {{0.f, -1.f}},
    {{0.f, 1.f}},
    {{-1.f, 1.f}},
};

// The right half, deliberately wound the other way: a back face.
constexpr QuadVertex backQuad[] = {
    {{0.f, 1.f}},
    {{1.f, -1.f}},
    {{0.f, -1.f}},
    {{1.f, 1.f}},
    {{1.f, -1.f}},
    {{0.f, 1.f}},
};

struct FlatShader final : ShaderProgram
{
    FlatShader() { compile(); }

    void define() override
    {
        auto position = vertexInput(&QuadVertex::position);

        setPosition(float4(position, 0.f, 1.f));
        setFragment(color);
    }

    Uniform<Float4> color;

    EACP_SHADER(color)
};

struct CullView final : GPUView
{
    explicit CullView(CullMode modeToUse)
    {
        setSampleCount(1);

        auto descriptor = RenderPipelineDescriptor {};
        descriptor.sampleCount = sampleCount();
        descriptor.cullMode = modeToUse;

        front.color = Array {1.f, 0.f, 0.f, 1.f};
        front.setVertices(frontQuad);
        front.prepare(descriptor);

        back.color = Array {0.f, 1.f, 0.f, 1.f};
        back.setVertices(backQuad);
        back.prepare(descriptor);
    }

    void render(Frame& frame) override
    {
        auto pass = frame.beginPass({{0.f, 0.f, 0.f, 1.f}});
        pass.draw(front);
        pass.draw(back);
    }

    FlatShader front;
    FlatShader back;
};

struct Halves
{
    Graphics::Color left;
    Graphics::Color right;
};

Halves render(CullMode mode)
{
    auto view = CullView {mode};
    view.setBounds({0.f, 0.f, (float) viewWidth, (float) viewHeight});

    auto image = view.renderToImage(1.f);
    check(image.isValid());

    return {image.at(viewWidth / 4, viewHeight / 2),
            image.at(viewWidth * 3 / 4, viewHeight / 2)};
}

bool isRed(const Graphics::Color& color)
{
    return color.r > 0.5f && color.g < 0.5f;
}

bool isGreen(const Graphics::Color& color)
{
    return color.g > 0.5f && color.r < 0.5f;
}

bool isCleared(const Graphics::Color& color)
{
    return color.r < 0.5f && color.g < 0.5f;
}
} // namespace

auto tCullNoneDrawsBoth = test("CullMode/noneDrawsBothWindings") = []
{
    if (!Device::shared().isValid())
        return;

    auto halves = render(CullMode::None);

    check(isRed(halves.left));
    check(isGreen(halves.right));
};

auto tCullBackKeepsTheFrontFace = test("CullMode/backKeepsTheFrontFace") = []
{
    if (!Device::shared().isValid())
        return;

    auto halves = render(CullMode::Back);

    check(isRed(halves.left));
    check(isCleared(halves.right));
};

auto tCullFrontKeepsTheBackFace = test("CullMode/frontKeepsTheBackFace") = []
{
    if (!Device::shared().isValid())
        return;

    auto halves = render(CullMode::Front);

    check(isCleared(halves.left));
    check(isGreen(halves.right));
};

// Culling is encoder state on Metal, so the mode has to be set on every
// setPipeline rather than only on the pipelines that cull.
auto tCullDoesNotLeakBetweenPipelines =
    test("CullMode/doesNotLeakToTheNextDraw") = []
{
    if (!Device::shared().isValid())
        return;

    struct MixedView final : GPUView
    {
        MixedView()
        {
            setSampleCount(1);

            auto culling = RenderPipelineDescriptor {};
            culling.sampleCount = sampleCount();
            culling.cullMode = CullMode::Back;

            // Here to leave a cull mode behind, not to be seen.
            culled.color = Array {0.f, 0.f, 1.f, 1.f};
            culled.setVertices(backQuad);
            culled.prepare(culling);

            auto plain = RenderPipelineDescriptor {};
            plain.sampleCount = sampleCount();

            kept.color = Array {0.f, 1.f, 0.f, 1.f};
            kept.setVertices(backQuad);
            kept.prepare(plain);
        }

        void render(Frame& frame) override
        {
            auto pass = frame.beginPass({{0.f, 0.f, 0.f, 1.f}});
            pass.draw(culled);
            pass.draw(kept);
        }

        FlatShader culled;
        FlatShader kept;
    };

    auto view = MixedView {};
    view.setBounds({0.f, 0.f, (float) viewWidth, (float) viewHeight});

    auto image = view.renderToImage(1.f);
    check(image.isValid());

    check(isGreen(image.at(viewWidth * 3 / 4, viewHeight / 2)));
};
