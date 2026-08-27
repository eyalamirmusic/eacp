#include "Common.h"

// The stencil buffer: the attachment, the per-face state on the pipeline, and
// the reference value on the pass.
//
// Checked by drawing, like the cull modes and the blend modes and for the same
// reason: a stencil buffer has no CPU-side observable, and a pipeline that
// built says nothing about whether the test ran. So one scene draws twice - a
// writer that puts values into the buffer, then a tester that covers the whole
// frame and only survives where the buffer says it may - and each case reads
// back which halves of the frame the tester reached.
//
// Reading the colours: the frame clears to black, the writer paints blue and
// the tester green. Green is the tester passing, blue is the tester masked
// where the writer had been, black is neither drawing. Three outcomes rather
// than two, so a case that passes cannot be one that drew nothing.
//
// **Every case here has its mirror**, which is what the cull-mode work taught:
// a stencil test that always fails would pass any test that only looks for
// something missing, and a front/back convention can be checked only by
// swapping the faces and finding the other half of the frame. Nothing below is
// asserted in one direction alone.
//
// What these do NOT distinguish, and it is the depth work's lesson one plane
// over: whether Frame::beginPass attached the stencil plane at all. On Apple
// silicon the tile memory is there either way, so a missing attachment can
// leave a rendering test green while Metal's validation layer objects at every
// draw. **Run this file under MTL_DEBUG_LAYER=1 when changing the attachment**;
// a silent validation layer is the other half of the evidence. D3D12 has no
// such luck, which is the second reason the Windows leg is worth its cost.
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
// Counter-clockwise in clip space - the space setPosition writes, with y up -
// so these are front faces under eacp's convention. See CullMode's note.
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

// The left half front-facing and the right half wound the other way round, in
// one buffer so a single draw hands the rasterizer both facings and the
// pipeline's two stencil faces are told apart by geometry alone.
constexpr QuadVertex bothFacings[] = {
    {{-1.f, -1.f}},
    {{0.f, -1.f}},
    {{-1.f, 1.f}},
    {{0.f, -1.f}},
    {{0.f, 1.f}},
    {{-1.f, 1.f}},

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

        setPosition(float4(position, depth, 1.f));
        setFragment(color);
    }

    Uniform<Float> depth;
    Uniform<Float4> color;

    EACP_SHADER(depth, color)
};

// What one case does differently. Everything here is either pipeline state on
// one of the two draws or pass state around them, so a case changes exactly the
// field it is about and nothing else moves.
struct StencilScene
{
    const QuadVertex* writerVertices = leftQuad;
    int writerVertexCount = 6;
    bool drawWriter = true;

    unsigned char clearStencil = 0;

    StencilFace writerFront;
    StencilFace writerBack;
    unsigned char writeMask = 0xff;
    unsigned int writerRef = 1;

    CompareFunction testCompare = CompareFunction::Equal;
    unsigned char readMask = 0xff;
    unsigned int testRef = 1;
};

struct StencilView final : GPUView
{
    explicit StencilView(const StencilScene& sceneToUse)
        : scene(sceneToUse)
    {
        setSampleCount(1);
        setStencil(true);

        auto writerPipeline = RenderPipelineDescriptor {};
        writerPipeline.sampleCount = sampleCount();
        writerPipeline.stencil = true;
        writerPipeline.stencilFront = scene.writerFront;
        writerPipeline.stencilBack = scene.writerBack;
        writerPipeline.stencilWriteMask = scene.writeMask;

        writer.depth = 0.5f;
        writer.color = Array {0.f, 0.f, 1.f, 1.f};
        writer.setVertices(scene.writerVertices, scene.writerVertexCount);
        writer.prepare(writerPipeline);

        // The tester reads and never writes, which is the pass-through the
        // StencilFace defaults already are - only the comparison is set.
        auto testerPipeline = RenderPipelineDescriptor {};
        testerPipeline.sampleCount = sampleCount();
        testerPipeline.stencil = true;
        testerPipeline.stencilFront.compare = scene.testCompare;
        testerPipeline.stencilBack.compare = scene.testCompare;
        testerPipeline.stencilReadMask = scene.readMask;

        tester.depth = 0.5f;
        tester.color = Array {0.f, 1.f, 0.f, 1.f};
        tester.setVertices(fullQuad);
        tester.prepare(testerPipeline);
    }

    void render(Frame& frame) override
    {
        auto descriptor = RenderPassDescriptor {};
        descriptor.clearColor = Graphics::Color::black();
        descriptor.clearStencil = scene.clearStencil;

        auto pass = frame.beginPass(descriptor);

        if (scene.drawWriter)
        {
            pass.setStencilReference(scene.writerRef);
            pass.draw(writer);
        }

        pass.setStencilReference(scene.testRef);
        pass.draw(tester);
    }

    StencilScene scene;
    FlatShader writer;
    FlatShader tester;
};

struct Halves
{
    Graphics::Color left;
    Graphics::Color right;
};

Halves renderHalves(GPUView& view)
{
    view.setBounds({0.f, 0.f, (float) viewWidth, (float) viewHeight});

    auto image = view.renderToImage(1.f);
    check(image.isValid());

    return {image.at(viewWidth / 4, viewHeight / 2),
            image.at(viewWidth * 3 / 4, viewHeight / 2)};
}

Halves render(const StencilScene& scene)
{
    auto view = StencilView {scene};

    // Asserted rather than skipped past: a view that quietly came back without
    // the plane would make every case below vacuous.
    check(view.hasStencil());

    return renderHalves(view);
}

bool isGreen(const Graphics::Color& color)
{
    return color.g > 0.5f && color.r < 0.5f && color.b < 0.5f;
}

bool isBlue(const Graphics::Color& color)
{
    return color.b > 0.5f && color.g < 0.5f;
}

bool isCleared(const Graphics::Color& color)
{
    return color.r < 0.5f && color.g < 0.5f && color.b < 0.5f;
}

// The writer that simply stamps the reference value wherever it draws.
StencilFace replacing()
{
    auto face = StencilFace {};
    face.pass = StencilOp::Replace;
    return face;
}
} // namespace

// A view says whether it carries the plane, so a pipeline can be built to match
// rather than finding out at the draw. The plane lives in the depth attachment,
// so asking for it brings the depth buffer with it and dropping the attachment
// takes it away again.
auto tViewReportsItsStencil = test("Stencil/aViewSaysWhetherItHasOne") = []
{
    struct PlainView final : GPUView
    {
        void render(Frame&) override {}
    };

    auto view = PlainView {};

    check(!view.hasStencil());
    check(!view.hasDepth());

    view.setDepth(true);

    check(view.hasDepth());
    check(!view.hasStencil());

    view.setStencil(true);

    check(view.hasStencil());
    check(view.hasDepth());

    view.setDepth(false);

    check(!view.hasDepth());
    check(!view.hasStencil());
};

// The same question of a texture target, and the same answer: stencil implies
// depth, and neither is allocated for a texture that does not render.
auto tTargetReportsItsStencil = test("Stencil/aTargetSaysWhetherItHasOne") = []
{
    if (!Device::shared().isValid())
        return;

    auto descriptor = TextureDescriptor {};
    descriptor.width = 4;
    descriptor.height = 4;
    descriptor.renderTarget = true;
    descriptor.depth = true;

    auto depthOnly = Device::shared().makeTexture(descriptor);

    check(depthOnly.hasDepth());
    check(!depthOnly.hasStencil());

    descriptor.depth = false;
    descriptor.stencil = true;

    auto stencilled = Device::shared().makeTexture(descriptor);

    check(stencilled.hasStencil());
    check(stencilled.hasDepth());

    descriptor.renderTarget = false;

    auto plain = Device::shared().makeTexture(descriptor);

    check(plain.isValid());
    check(!plain.hasStencil());
    check(!plain.hasDepth());
};

// The mask itself: the writer stamps 1 over the left half, and the tester -
// which covers the whole frame - survives only there. The right half is the
// half that says the test ran, since a tester drawn everywhere would have
// painted it too.
auto tStencilConfinesTheSecondDraw =
    test("Stencil/theMaskConfinesTheSecondDraw") = []
{
    if (!Device::shared().isValid())
        return;

    auto scene = StencilScene {};
    scene.writerFront = replacing();
    scene.writerBack = replacing();

    auto halves = render(scene);

    check(isGreen(halves.left));
    check(isCleared(halves.right));
};

// The control, and what makes the case above evidence: the same two draws with
// the comparison set to Always cover the whole frame, so the missing half is
// the stencil test and not geometry that was never going to appear.
auto tWithoutTheTestBothHalvesDraw =
    test("Stencil/withoutTheTestTheWholeFrameDraws") = []
{
    if (!Device::shared().isValid())
        return;

    auto scene = StencilScene {};
    scene.writerFront = replacing();
    scene.writerBack = replacing();
    scene.testCompare = CompareFunction::Always;

    auto halves = render(scene);

    check(isGreen(halves.left));
    check(isGreen(halves.right));
};

// The reference is pass state, not pipeline state, and reaches the comparison:
// the same buffer tested against a different value passes nowhere, leaving the
// writer's own blue where it drew.
auto tReferenceIsPassState = test("Stencil/theReferenceIsWhatTheComparisonUses") = []
{
    if (!Device::shared().isValid())
        return;

    auto scene = StencilScene {};
    scene.writerFront = replacing();
    scene.writerBack = replacing();
    scene.testRef = 2;

    auto halves = render(scene);

    check(isBlue(halves.left));
    check(isCleared(halves.right));
};

// The plane starts each pass at the value the descriptor names rather than at
// whatever the last frame left in it. No writer at all here, so the clear is
// the only thing the tester can be reading.
auto tClearValueIsWhatTheTestSeesFirst =
    test("Stencil/thePassClearsToTheValueItNames") = []
{
    if (!Device::shared().isValid())
        return;

    auto cleared = StencilScene {};
    cleared.drawWriter = false;
    cleared.clearStencil = 1;

    auto passing = render(cleared);

    check(isGreen(passing.left));
    check(isGreen(passing.right));

    cleared.clearStencil = 0;

    auto failing = render(cleared);

    check(isCleared(failing.left));
    check(isCleared(failing.right));
};

// **The cross-backend convention.** One draw hands the rasterizer a
// front-facing left half and a back-facing right half, and the pipeline writes
// on one facing only - so which half of the frame survives is the whole of what
// "front" means here.
//
// Both directions, for the reason CullModeTests gives: a backend that had the
// two faces the wrong way round would pass a one-sided check by writing the
// other half, and only the mirror says which half it was.
//
// The unmarked half comes back blue rather than black, and that is the sharper
// result: the writer covers both halves, so blue says its geometry rasterized
// there and only the stencil write was withheld. Black would have left open
// that the half was never drawn at all.
auto tFrontAndBackFacesAreToldApart =
    test("Stencil/frontAndBackFacesAreToldApart") = []
{
    if (!Device::shared().isValid())
        return;

    auto scene = StencilScene {};
    scene.writerVertices = bothFacings;
    scene.writerVertexCount = 12;
    scene.writerFront = replacing();

    auto onFront = render(scene);

    check(isGreen(onFront.left));
    check(isBlue(onFront.right));

    scene.writerFront = StencilFace {};
    scene.writerBack = replacing();

    auto onBack = render(scene);

    check(isBlue(onBack.left));
    check(isGreen(onBack.right));
};

// Increment wraps rather than saturating, which is what a shadow volume counts
// on: from 255 it reaches 0. The clamping form is the mirror and stays at 255,
// so this also pins that the two are not swapped - D3D12 spells the wrapping
// pair INCR/DECR and the clamping pair INCR_SAT/DECR_SAT, which reads backwards
// from every other API and would go unnoticed until a pixel sat inside 255
// volumes.
auto tIncrementWraps = test("Stencil/incrementWrapsAndTheClampingFormDoesNot") = []
{
    if (!Device::shared().isValid())
        return;

    auto scene = StencilScene {};
    scene.writerVertices = fullQuad;
    scene.clearStencil = 255;
    scene.writerFront.pass = StencilOp::IncrementWrap;
    scene.writerBack.pass = StencilOp::IncrementWrap;
    scene.testRef = 0;

    auto wrapped = render(scene);

    check(isGreen(wrapped.left));
    check(isGreen(wrapped.right));

    scene.writerFront.pass = StencilOp::IncrementClamp;
    scene.writerBack.pass = StencilOp::IncrementClamp;

    auto clamped = render(scene);

    check(isBlue(clamped.left));
    check(isBlue(clamped.right));
};

// The write mask limits which bits a write may change, so a Replace of 0xff
// through a mask of 0x0f leaves 0x0f behind. Tested both ways round: the value
// that should be there passes and the one that would be there without the mask
// does not.
auto tWriteMaskLimitsWhichBitsChange =
    test("Stencil/theWriteMaskLimitsWhichBitsChange") = []
{
    if (!Device::shared().isValid())
        return;

    auto scene = StencilScene {};
    scene.writerVertices = fullQuad;
    scene.writerFront = replacing();
    scene.writerBack = replacing();
    scene.writerRef = 0xff;
    scene.writeMask = 0x0f;
    scene.testRef = 0x0f;

    auto masked = render(scene);

    check(isGreen(masked.left));
    check(isGreen(masked.right));

    scene.testRef = 0xff;

    auto unmasked = render(scene);

    check(isBlue(unmasked.left));
    check(isBlue(unmasked.right));
};

// The read mask limits which bits take part in the comparison, on both sides of
// it: 0xff against a stored 0x0f is unequal until the mask takes both down to
// the low nibble.
auto tReadMaskLimitsWhichBitsCompare =
    test("Stencil/theReadMaskLimitsWhichBitsAreCompared") = []
{
    if (!Device::shared().isValid())
        return;

    auto scene = StencilScene {};
    scene.writerVertices = fullQuad;
    scene.writerFront = replacing();
    scene.writerBack = replacing();
    scene.writerRef = 0x0f;
    scene.testRef = 0xff;

    auto whole = render(scene);

    check(isBlue(whole.left));
    check(isBlue(whole.right));

    scene.readMask = 0x0f;

    auto lowNibble = render(scene);

    check(isGreen(lowNibble.left));
    check(isGreen(lowNibble.right));
};

// depthFail is its own outcome, and the one the depth-fail shadow algorithm
// counts - so a stencil op reached only by failing the depth test has to be
// wired to that outcome and not to the pass.
//
// A near quad covers the left half and writes depth. A full-frame quad then
// tests Less against it: on the left it fails, on the right it passes. Putting
// Replace on depthFail therefore marks the left half, and putting it on pass
// marks the right - which is the mirror, and the reason either is evidence.
//
// The half the tester does not reach is blue on the left and black on the
// right, and the asymmetry is the depth test doing its job rather than noise:
// the volume is discarded over the near quad, so the quad's own blue survives
// there, while on the right the volume draws its black over the clear.
auto tDepthFailIsItsOwnOutcome = test("Stencil/depthFailIsItsOwnOutcome") = []
{
    if (!Device::shared().isValid())
        return;

    struct DepthFailView final : GPUView
    {
        explicit DepthFailView(bool markOnDepthFail)
        {
            setSampleCount(1);
            setStencil(true);

            auto occluderPipeline = RenderPipelineDescriptor {};
            occluderPipeline.sampleCount = sampleCount();
            occluderPipeline.stencil = true;
            occluderPipeline.depth = true;

            occluder.depth = 0.25f;
            occluder.color = Array {0.f, 0.f, 1.f, 1.f};
            occluder.setVertices(leftQuad);
            occluder.prepare(occluderPipeline);

            // Reads the depth it was handed and writes none of its own, so the
            // only thing it changes is the stencil plane.
            auto volumePipeline = RenderPipelineDescriptor {};
            volumePipeline.sampleCount = sampleCount();
            volumePipeline.stencil = true;
            volumePipeline.depth = true;
            volumePipeline.depthCompare = CompareFunction::Less;
            volumePipeline.depthWrite = false;

            auto marking = StencilOp::Replace;

            if (markOnDepthFail)
                volumePipeline.stencilFront.depthFail = marking;
            else
                volumePipeline.stencilFront.pass = marking;

            volumePipeline.stencilBack = volumePipeline.stencilFront;

            volume.depth = 0.75f;
            volume.color = Array {0.f, 0.f, 0.f, 1.f};
            volume.setVertices(fullQuad);
            volume.prepare(volumePipeline);

            auto testerPipeline = RenderPipelineDescriptor {};
            testerPipeline.sampleCount = sampleCount();
            testerPipeline.stencil = true;
            testerPipeline.stencilFront.compare = CompareFunction::Equal;
            testerPipeline.stencilBack.compare = CompareFunction::Equal;

            tester.depth = 0.f;
            tester.color = Array {0.f, 1.f, 0.f, 1.f};
            tester.setVertices(fullQuad);
            tester.prepare(testerPipeline);
        }

        void render(Frame& frame) override
        {
            auto pass = frame.beginPass({Graphics::Color::black()});

            pass.setStencilReference(1);
            pass.draw(occluder);
            pass.draw(volume);
            pass.draw(tester);
        }

        FlatShader occluder;
        FlatShader volume;
        FlatShader tester;
    };

    auto onDepthFail = DepthFailView {true};
    auto marked = renderHalves(onDepthFail);

    check(isGreen(marked.left));
    check(isCleared(marked.right));

    auto onPass = DepthFailView {false};
    auto passed = renderHalves(onPass);

    check(isBlue(passed.left));
    check(isGreen(passed.right));
};

// The reference does not survive a pass, on either backend. It is encoder state
// on Metal - which resets it to zero at every pass - and command-list state on
// D3D12, where one list carries several passes and would lend the value on.
//
// The divergence is invisible until a frame has two passes, which is why this
// case has one: the first renders into a texture with a reference of 1, and the
// second draws on the drawable comparing Equal without naming a reference at
// all. The drawable's plane is freshly cleared to zero, so the draw survives
// only if the comparison is against zero too.
auto tReferenceDoesNotSurviveAPass =
    test("Stencil/theReferenceDoesNotSurviveAPass") = []
{
    if (!Device::shared().isValid())
        return;

    struct TwoPassView final : GPUView
    {
        TwoPassView()
            : target(Device::shared().makeTexture(describe()))
        {
            setSampleCount(1);
            setStencil(true);

            // Into the texture, with a reference of 1 to leave behind.
            auto intoTarget = RenderPipelineDescriptor {};
            intoTarget.sampleCount = 1;
            intoTarget.stencil = true;
            intoTarget.stencilFront = replacing();
            intoTarget.stencilBack = replacing();
            intoTarget.colorFormat = pixelFormatFor(TextureFormat::RGBA8Unorm);

            offscreen.depth = 0.5f;
            offscreen.color = Array {0.f, 0.f, 1.f, 1.f};
            offscreen.setVertices(fullQuad);
            offscreen.prepare(intoTarget);

            // On the drawable, comparing against whatever reference the pass
            // starts with.
            auto onDrawable = RenderPipelineDescriptor {};
            onDrawable.sampleCount = sampleCount();
            onDrawable.stencil = true;
            onDrawable.stencilFront.compare = CompareFunction::Equal;
            onDrawable.stencilBack.compare = CompareFunction::Equal;

            tester.depth = 0.5f;
            tester.color = Array {0.f, 1.f, 0.f, 1.f};
            tester.setVertices(fullQuad);
            tester.prepare(onDrawable);
        }

        static TextureDescriptor describe()
        {
            auto descriptor = TextureDescriptor {};
            descriptor.width = 4;
            descriptor.height = 4;
            descriptor.format = TextureFormat::RGBA8Unorm;
            descriptor.renderTarget = true;
            descriptor.stencil = true;
            return descriptor;
        }

        void render(Frame& frame) override
        {
            {
                auto into = frame.beginPass(target, {Graphics::Color::black()});
                into.setStencilReference(1);
                into.draw(offscreen);
            }

            auto pass = frame.beginPass({Graphics::Color::black()});
            pass.draw(tester);
        }

        Texture target;
        FlatShader offscreen;
        FlatShader tester;
    };

    auto view = TwoPassView {};

    check(view.target.hasStencil());

    auto halves = renderHalves(view);

    check(isGreen(halves.left));
    check(isGreen(halves.right));
};
