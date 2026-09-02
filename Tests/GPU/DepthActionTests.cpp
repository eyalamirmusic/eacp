#include "Common.h"

// RenderPassDescriptor::depthAction: whether the depth and stencil planes
// survive the end of a pass.
//
// The case that wants them to is a *suspended* pass. A texture cannot be
// sampled by the pass rendering into it, so an app that has to copy the frame
// it has drawn so far - a refraction reading what is behind it, Doom 3's
// _currentRender - ends its pass, copies, and opens another over the same
// attachments. `clear = false` already keeps the colour across that boundary;
// this is the other half, and without it everything drawn after the copy has an
// empty depth buffer to be occluded by.
//
// Checked by drawing, like the stencil and cull work beside it, because neither
// plane has a CPU-side observable: one pass writes into the buffer, a second
// pass over the same frame draws something the buffer should reject, and what
// comes back says whether it was rejected.
//
// **Every case has its mirror**, and here the mirror is the whole point rather
// than a discipline: a test that only asserts the second draw was rejected
// passes just as well when nothing drew at all. So the same two passes run
// again with DepthAction::Clear - the behaviour before this existed - and have
// to come back the *other* colour.
//
// The third case is what separates the load from the store: Keep then Clear
// stores a buffer nothing loads, and must look exactly like Clear then Clear.
// Clear then Resume is deliberately absent, being the one combination with no
// answer - it loads on Metal what the pass before it was told to discard.
//
// **The stencil case is not the depth case one plane over, and taking it out
// would leave half of this untested.** Measured while writing it: putting the
// store back to MTLStoreActionDontCare - so the buffer is loaded but nothing
// kept it - leaves every depth case here green on Apple silicon, the tile
// memory holding the values across the boundary anyway, and fails the stencil
// one. That is the same architecture-dependent silence RenderTargetDepthTests
// warns about at the attachment, one step along: the depth half alone would
// report a store nobody needed as working.
//
// Runs on both backends, and self-skips without a GPU device.

using namespace nano;
using namespace eacp;
using namespace eacp::GPU;

namespace
{
constexpr auto viewWidth = 8;
constexpr auto viewHeight = 4;

// Clip-space z for the two draws. Both inside [0, 1] with w = 1, so they reach
// the depth buffer as they stand and near really is nearer.
constexpr auto nearDepth = 0.25f;
constexpr auto farDepth = 0.75f;

struct QuadVertex
{
    float position[2];
};
} // namespace

EACP_SHADER_VALUE(QuadVertex, Float2)

namespace
{
// Counter-clockwise in clip space - the space setPosition writes, with y up -
// so these are front faces under eacp's convention.
constexpr QuadVertex fullQuad[] = {
    {{-1.f, -1.f}},
    {{1.f, -1.f}},
    {{-1.f, 1.f}},
    {{1.f, -1.f}},
    {{1.f, 1.f}},
    {{-1.f, 1.f}},
};

constexpr QuadVertex leftQuad[] = {
    {{-1.f, -1.f}},
    {{0.f, -1.f}},
    {{-1.f, 1.f}},
    {{0.f, -1.f}},
    {{0.f, 1.f}},
    {{-1.f, 1.f}},
};

struct FlatShader final : ShaderProgram
{
    FlatShader() { compile(); }

    void define() override
    {
        auto position = vertexInput(&QuadVertex::position);

        setPosition(float4(position, depth, 1.f));
        setFragment(color);
    }

    Uniform<Float> depth;
    Uniform<Float4> color;

    EACP_SHADER(depth, color)
};

// The two passes' actions, which is the whole of what a case varies. The second
// pass never clears its colour, so what it inherits is only ever in question
// for the two planes this is about.
struct Suspension
{
    DepthAction first = DepthAction::Keep;
    DepthAction second = DepthAction::Resume;
};

// Near quad, then - across a pass boundary - a far one over the whole frame.
// Blue is the near quad surviving, which only a depth buffer that crossed the
// boundary does; green is the far one painting over it.
struct DepthView final : GPUView
{
    explicit DepthView(const Suspension& suspensionToUse)
        : suspension(suspensionToUse)
    {
        setSampleCount(1);
        setDepth(true);

        auto descriptor = RenderPipelineDescriptor {};
        descriptor.sampleCount = sampleCount();
        descriptor.depth = true;
        descriptor.depthCompare = CompareFunction::Less;
        descriptor.depthWrite = true;

        near.depth = nearDepth;
        near.color = Array {0.f, 0.f, 1.f, 1.f};
        near.setVertices(fullQuad);
        near.prepare(descriptor);

        far.depth = farDepth;
        far.color = Array {0.f, 1.f, 0.f, 1.f};
        far.setVertices(fullQuad);
        far.prepare(descriptor);
    }

    void render(Frame& frame) override
    {
        {
            auto descriptor = RenderPassDescriptor {};
            descriptor.clearColor = Graphics::Color::black();
            descriptor.depthAction = suspension.first;

            auto pass = frame.beginPass(descriptor);
            pass.draw(near);
        }

        auto descriptor = RenderPassDescriptor {};
        descriptor.clear = false;
        descriptor.depthAction = suspension.second;

        auto pass = frame.beginPass(descriptor);
        pass.draw(far);
    }

    Suspension suspension;
    FlatShader near;
    FlatShader far;
};

// The same shape one plane over: the left half stamped with a stencil value in
// the first pass, and a full-frame draw in the second that only survives where
// the value is still there.
struct StencilView final : GPUView
{
    explicit StencilView(const Suspension& suspensionToUse)
        : suspension(suspensionToUse)
    {
        setSampleCount(1);
        setStencil(true);

        auto writerPipeline = RenderPipelineDescriptor {};
        writerPipeline.sampleCount = sampleCount();
        writerPipeline.stencil = true;
        writerPipeline.stencilFront.pass = StencilOp::Replace;
        writerPipeline.stencilBack.pass = StencilOp::Replace;

        writer.depth = 0.5f;
        writer.color = Array {0.f, 0.f, 1.f, 1.f};
        writer.setVertices(leftQuad);
        writer.prepare(writerPipeline);

        auto testerPipeline = RenderPipelineDescriptor {};
        testerPipeline.sampleCount = sampleCount();
        testerPipeline.stencil = true;
        testerPipeline.stencilFront.compare = CompareFunction::Equal;
        testerPipeline.stencilBack.compare = CompareFunction::Equal;

        tester.depth = 0.5f;
        tester.color = Array {0.f, 1.f, 0.f, 1.f};
        tester.setVertices(fullQuad);
        tester.prepare(testerPipeline);
    }

    void render(Frame& frame) override
    {
        {
            auto descriptor = RenderPassDescriptor {};
            descriptor.clearColor = Graphics::Color::black();
            descriptor.depthAction = suspension.first;

            auto pass = frame.beginPass(descriptor);
            pass.setStencilReference(1);
            pass.draw(writer);
        }

        auto descriptor = RenderPassDescriptor {};
        descriptor.clear = false;
        descriptor.depthAction = suspension.second;

        auto pass = frame.beginPass(descriptor);
        pass.setStencilReference(1);
        pass.draw(tester);
    }

    Suspension suspension;
    FlatShader writer;
    FlatShader tester;
};

Graphics::Color centrePixel(GPUView& view)
{
    view.setBounds({0.f, 0.f, (float) viewWidth, (float) viewHeight});

    auto image = view.renderToImage(1.f);
    check(image.isValid());

    return image.at(viewWidth / 2, viewHeight / 2);
}

Graphics::Color leftPixel(GPUView& view)
{
    view.setBounds({0.f, 0.f, (float) viewWidth, (float) viewHeight});

    auto image = view.renderToImage(1.f);
    check(image.isValid());

    return image.at(viewWidth / 4, viewHeight / 2);
}

bool isBlue(const Graphics::Color& color)
{
    return color.b > 0.5f && color.g < 0.5f;
}

bool isGreen(const Graphics::Color& color)
{
    return color.g > 0.5f && color.b < 0.5f;
}

Graphics::Color depthResult(const Suspension& suspension)
{
    auto view = DepthView {suspension};

    // Asserted rather than skipped past: a view that quietly came back without
    // the buffer would make every case here vacuous.
    check(view.hasDepth());

    return centrePixel(view);
}

Graphics::Color stencilResult(const Suspension& suspension)
{
    auto view = StencilView {suspension};

    check(view.hasStencil());

    return leftPixel(view);
}
} // namespace

// The case this exists for: the depth the first pass wrote is still there in
// the second, so the farther draw is rejected and the nearer one's colour
// stands.
auto tResumeKeepsTheDepth = test("DepthAction/aResumedPassKeepsTheDepthBuffer") = []
{
    if (!Device::shared().isValid())
        return;

    check(isBlue(depthResult({DepthAction::Keep, DepthAction::Resume})));
};

// The mirror, and the reason the case above is evidence rather than a draw that
// silently did nothing: the same two passes with the default action clear the
// buffer at the boundary, so the farther draw survives and comes back the other
// colour.
auto tClearLosesTheDepth = test("DepthAction/aClearingPassStartsEmpty") = []
{
    if (!Device::shared().isValid())
        return;

    check(isGreen(depthResult({DepthAction::Clear, DepthAction::Clear})));
};

// Keep is the store and Resume is the load, and only the load decides what a
// pass starts from: a buffer kept by the pass before one that clears is a
// buffer nothing reads.
auto tKeepAloneChangesNothing =
    test("DepthAction/keepingABufferNothingLoadsChangesNothing") = []
{
    if (!Device::shared().isValid())
        return;

    check(isGreen(depthResult({DepthAction::Keep, DepthAction::Clear})));
};

// The stencil plane crosses the boundary with the depth, the two being one
// attachment of one format - so the mask the first pass stamped is what the
// second pass tests against.
auto tResumeKeepsTheStencil =
    test("DepthAction/aResumedPassKeepsTheStencilPlane") = []
{
    if (!Device::shared().isValid())
        return;

    check(isGreen(stencilResult({DepthAction::Keep, DepthAction::Resume})));
};

// Its mirror: cleared at the boundary, the mask is gone and the tester survives
// nowhere, leaving the writer's own colour where it drew.
auto tClearLosesTheStencil = test("DepthAction/aClearingPassEmptiesTheStencil") = []
{
    if (!Device::shared().isValid())
        return;

    check(isBlue(stencilResult({DepthAction::Clear, DepthAction::Clear})));
};
