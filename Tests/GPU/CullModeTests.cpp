#include "Common.h"

// RenderPipelineDescriptor::cullMode, and the winding convention that gives it
// its meaning.
//
// Checked by drawing, like the blend modes and for the same reason: culling has
// no CPU-side observable, and a pipeline that built says nothing about whether
// a face was thrown away. So one scene holds two quads of opposite winding,
// side by side, and each case reads back which of them survived.
//
// That scene is what makes any of the cases evidence. CullMode::None is the
// control: both quads are drawn, so a later case finding one missing knows the
// difference is the cull mode and not a quad that was never going to appear.
// And because Front and Back keep *different* quads, neither can pass by
// drawing nothing.
//
// **This file is also where the two backends are held to one convention**, and
// that is worth more than the coverage. Both backends default to "clockwise is
// front-facing", so eacp states the convention in clip space instead -
// counter-clockwise is front, as glTF has it - sets each backend to whatever
// produces that, and this file is what fails if either drifts.
//
// It has already done that job once. The D3D12 half was originally set from the
// reasoning that D3D12 decides facing in screen space, after a y flip Metal does
// not have, and so needed the opposite setting; on that reasoning it went to
// FrontCounterClockwise = FALSE and these two cases culled the opposite face on
// Windows. There is no such extra flip - clip-space y is up and the framebuffer
// origin is top left on both APIs - and the setting is now TRUE, the same
// convention Metal spells MTLWindingCounterClockwise. Both halves are measured
// now, which is the only reason either is trustworthy.
//
// Runs on both backends; self-skips without a GPU device.

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
// The left half of the frame, wound counter-clockwise in clip space - the space
// setPosition writes, with y up - so it is the front face: bottom left, bottom
// right, top left. CullMode::Back keeps it and CullMode::Front does not.
constexpr QuadVertex frontQuad[] = {
    {{-1.f, -1.f}},
    {{0.f, -1.f}},
    {{-1.f, 1.f}},
    {{0.f, -1.f}},
    {{0.f, 1.f}},
    {{-1.f, 1.f}},
};

// The right half, wound the other way round - the same corners per triangle in
// reverse order, which is the whole of what makes it a back face.
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

// Both quads through the same pipeline settings, so the cull mode is the only
// thing separating the two draws.
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

// The control, and the one that makes the others mean anything: with no culling
// both quads reach the frame, whichever way they are wound.
auto tCullNoneDrawsBoth = test("CullMode/noneDrawsBothWindings") = []
{
    if (!Device::shared().isValid())
        return;

    auto halves = render(CullMode::None);

    check(isRed(halves.left));
    check(isGreen(halves.right));
};

// Back culling keeps the front face, which is the counter-clockwise-in-clip-
// space one, so the left quad survives and the right is gone. Half the
// assertion is that the left one is still there: a cull mode that dropped
// everything would pass a test that only looked for what is missing.
auto tCullBackKeepsTheFrontFace = test("CullMode/backKeepsTheFrontFace") = []
{
    if (!Device::shared().isValid())
        return;

    auto halves = render(CullMode::Back);

    check(isRed(halves.left));
    check(isCleared(halves.right));
};

// And the mirror image, which is what pins the convention rather than merely
// pinning that something got culled: reversing the mode has to reverse which
// quad is left, not drop both.
auto tCullFrontKeepsTheBackFace = test("CullMode/frontKeepsTheBackFace") = []
{
    if (!Device::shared().isValid())
        return;

    auto halves = render(CullMode::Front);

    check(isCleared(halves.left));
    check(isGreen(halves.right));
};

// Culling is encoder state on Metal, so the mode has to be set on every
// setPipeline rather than only on the pipelines that cull: a culling draw
// followed by a non-culling one would otherwise leave the second missing its
// back faces. Two pipelines in one pass, the culling one first.
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

            // Culled away entirely: a back face under back culling. It is here
            // to leave a cull mode behind, not to be seen.
            culled.color = Array {0.f, 0.f, 1.f, 1.f};
            culled.setVertices(backQuad);
            culled.prepare(culling);

            auto plain = RenderPipelineDescriptor {};
            plain.sampleCount = sampleCount();

            // The same back-facing geometry with no culling. It must appear.
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
