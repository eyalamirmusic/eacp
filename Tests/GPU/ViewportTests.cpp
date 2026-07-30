#include "Common.h"

// RenderPass::setViewport / clearViewport, checked by drawing through them and
// reading the pixels back.
//
// The property that matters is the one that separates a viewport from a
// scissor, and it is easy to write a case that cannot tell them apart: a
// full-screen quad drawn through a half-target viewport and a full-screen quad
// drawn through a half-target scissor produce the same picture. So the geometry
// here deliberately covers only *half of clip space* - a viewport moves it and
// a scissor at the same rectangle would delete it, which is a difference no
// implementation can fake.
//
// Everything renders off-screen through View::renderToImage, so it runs in CI
// on both backends.

using namespace nano;
using namespace eacp;
using namespace eacp::GPU;

namespace
{
bool isGreen(const Graphics::Color& c)
{
    return c.g > 0.5f && c.r < 0.5f && c.b < 0.5f;
}

bool isRed(const Graphics::Color& c)
{
    return c.r > 0.5f && c.g < 0.5f && c.b < 0.5f;
}

// The clear colour, and so "nothing was drawn here".
bool isBlue(const Graphics::Color& c)
{
    return c.b > 0.5f && c.r < 0.5f && c.g < 0.5f;
}

const char* mslShader = R"(
#include <metal_stdlib>
using namespace metal;

struct VertexIn
{
    float3 position [[attribute(0)]];
    float4 color [[attribute(1)]];
};

struct VertexOut
{
    float4 position [[position]];
    float4 color;
};

vertex VertexOut vertexMain(VertexIn in [[stage_in]])
{
    VertexOut out;
    out.position = float4(in.position, 1.0);
    out.color = in.color;
    return out;
}

fragment float4 fragmentMain(VertexOut in [[stage_in]]) { return in.color; }
)";

const char* hlslShader = R"(
struct VertexIn
{
    float3 position : TEXCOORD0;
    float4 color : TEXCOORD1;
};

struct VertexOut
{
    float4 position : SV_Position;
    float4 color : TEXCOORD0;
};

VertexOut vertexMain(VertexIn input)
{
    VertexOut output;
    output.position = float4(input.position, 1.0);
    output.color = input.color;
    return output;
}

float4 fragmentMain(VertexOut input) : SV_Target { return input.color; }
)";

ShaderSource shaderSource()
{
    return Platform::isWindows() ? ShaderSource::hlsl(hlslShader)
                                 : ShaderSource::msl(mslShader);
}

struct Vertex
{
    float x, y, z;
    float r, g, b, a;
};

constexpr Vertex green(float x, float y, float z)
{
    return {x, y, z, 0.f, 1.f, 0.f, 1.f};
}

constexpr Vertex red(float x, float y, float z)
{
    return {x, y, z, 1.f, 0.f, 0.f, 1.f};
}

// The first quad is the whole point: it covers x from -1 to 0 in clip space -
// the left half - and nothing else. Where it lands on the target is the answer
// to every question in this file. The other two fill clip space at two depths,
// for the near/far range.
// clang-format off
constexpr Vertex geometry[] = {
    green(-1.f, -1.f, 0.f), green(0.f, -1.f, 0.f), green(0.f, 1.f, 0.f),
    green(-1.f, -1.f, 0.f), green(0.f,  1.f, 0.f), green(-1.f, 1.f, 0.f),

    red(-1.f, -1.f, 0.f), red(1.f, -1.f, 0.f), red(1.f, 1.f, 0.f),
    red(-1.f, -1.f, 0.f), red(1.f,  1.f, 0.f), red(-1.f, 1.f, 0.f),

    green(-1.f, -1.f, 0.25f), green(1.f, -1.f, 0.25f), green(1.f, 1.f, 0.25f),
    green(-1.f, -1.f, 0.25f), green(1.f,  1.f, 0.25f), green(-1.f, 1.f, 0.25f),
};
// clang-format on

constexpr auto leftHalfOfClipSpace = 0;
constexpr auto atTheNearPlane = 6;
constexpr auto aQuarterBack = 12;

// One draw and the viewport it is drawn through. `viewport` is applied when the
// rect is non-empty, `clearViewport` restores the whole target, and neither
// happening is what "the default" means.
struct Step
{
    int firstVertex = 0;
    int vertexCount = 6;

    Graphics::Rect viewport = {};
    float nearDepth = 0.f;
    float farDepth = 1.f;
    bool clearViewport = false;
};

// The target is 40x40, so a half is 20 pixels and the quarters fall on 10, 20
// and 30 - every assertion below is a whole number of pixels rather than a
// rounding argument.
constexpr auto targetSize = 40.f;

struct ViewportView final : GPUView
{
    explicit ViewportView(bool depth)
        : library(Device::shared().makeShaderLibrary(
              shaderSource().withVertex("vertexMain").withFragment("fragmentMain")))
        , vertexBuffer(Device::shared().makeBuffer(geometry))
        , pipeline(makePipeline(depth))
    {
    }

    RenderPipeline makePipeline(bool depth)
    {
        // Both settings belong here rather than in the constructor body: the
        // pipeline is built in the initialiser list and has to agree with the
        // target it will draw into, so the target has to be configured first.
        //
        // Single-sampled because MSAA would feather the boundary a moved quad
        // lands on and make every "which pixel" assertion approximate.
        setSampleCount(1);
        setDepth(depth);

        auto descriptor = RenderPipelineDescriptor {};
        descriptor.library = &library;
        descriptor.sampleCount = sampleCount();
        descriptor.vertexLayout.attribute(VertexFormat::Float3, 0)
            .attribute(VertexFormat::Float4, sizeof(float) * 3);
        descriptor.vertexLayout.stride = sizeof(Vertex);
        descriptor.depth = depth;

        return Device::shared().makeRenderPipeline(descriptor);
    }

    void render(Frame& frame) override
    {
        auto pass = frame.beginPass({{0.f, 0.f, 1.f, 1.f}});

        for (const auto& step: steps)
        {
            if (step.clearViewport)
                pass.clearViewport();
            else if (step.viewport.w > 0.f || step.viewport.h > 0.f)
                pass.setViewport(step.viewport, step.nearDepth, step.farDepth);

            pass.setPipeline(pipeline);
            pass.setVertexBuffer(vertexBuffer);
            pass.draw(step.vertexCount, step.firstVertex);
        }
    }

    Vector<Step> steps;

    ShaderLibrary library;
    Buffer vertexBuffer;
    RenderPipeline pipeline;
};

Graphics::Image renderSteps(const Vector<Step>& steps, bool depth = false)
{
    auto view = ViewportView {depth};
    view.steps = steps;
    view.setBounds({0.f, 0.f, targetSize, targetSize});

    return view.renderToImage(1.f);
}

// The row every case samples: halfway down, where all this geometry is solid.
constexpr auto middleRow = 20;
} // namespace

// The baseline, and the thing every other case is measured against: with no
// viewport set, clip space maps onto the whole target, so geometry covering the
// left half of clip space covers the left half of the target.
auto tDefaultViewportIsTheWholeTarget = test("Viewport/defaultIsTheWholeTarget") = []
{
    if (!Device::shared().isValid())
        return;

    const auto image = renderSteps({{.firstVertex = leftHalfOfClipSpace}});

    if (!image.isValid())
        return;

    check(isGreen(image.at(5, middleRow)));
    check(isGreen(image.at(19, middleRow)));
    check(isBlue(image.at(20, middleRow)));
    check(isBlue(image.at(35, middleRow)));
};

// The case this file exists for. The viewport is the target's right half, and
// the geometry is the left half of clip space - so it lands in the left half of
// the *viewport*, which is the target's third quarter, x from 20 to 30.
//
// A scissor set to the same rectangle would have produced an empty image: the
// geometry it would be clipping against is entirely on the other side. Nothing
// but a real viewport transform puts green at x = 25.
auto tViewportMovesGeometryRatherThanClipping =
    test("Viewport/movesGeometryRatherThanClipping") = []
{
    if (!Device::shared().isValid())
        return;

    const auto image = renderSteps(
        {{.firstVertex = leftHalfOfClipSpace, .viewport = {20.f, 0.f, 20.f, 40.f}}});

    if (!image.isValid())
        return;

    check(isBlue(image.at(5, middleRow))); // where it would have been
    check(isBlue(image.at(19, middleRow)));
    check(isGreen(image.at(21, middleRow))); // where the viewport put it
    check(isGreen(image.at(29, middleRow)));
    check(isBlue(image.at(31, middleRow))); // the viewport's own right half
    check(isBlue(image.at(38, middleRow)));
};

// The y axis too, and in the same direction as everything else in this library:
// a viewport at the top half puts geometry filling clip space in the top half
// of the target, because a viewport rect is y-down like Graphics::Rect and like
// setScissorRect. A backend that took y from the bottom would put it at the
// other end and no x-axis case would notice.
auto tViewportYIsMeasuredFromTheTop = test("Viewport/yIsMeasuredFromTheTop") = []
{
    if (!Device::shared().isValid())
        return;

    const auto image = renderSteps(
        {{.firstVertex = atTheNearPlane, .viewport = {0.f, 0.f, 40.f, 20.f}}});

    if (!image.isValid())
        return;

    check(isRed(image.at(20, 5)));
    check(isRed(image.at(20, 19)));
    check(isBlue(image.at(20, 21)));
    check(isBlue(image.at(20, 35)));
};

// Two draws, one pass: the viewport is state that stays set until something
// changes it, and clearViewport is what changes it back. Both halves of the
// picture have to be there at the end - the moved quad from the first draw and
// the unmoved one from the second.
auto tClearViewportRestoresTheWholeTarget =
    test("Viewport/clearRestoresTheWholeTarget") = []
{
    if (!Device::shared().isValid())
        return;

    const auto image = renderSteps({
        {.firstVertex = leftHalfOfClipSpace, .viewport = {20.f, 0.f, 20.f, 40.f}},
        {.firstVertex = leftHalfOfClipSpace, .clearViewport = true},
    });

    if (!image.isValid())
        return;

    // The two land side by side rather than with a gap: the restored draw
    // covers 0..20 and the moved one 20..30, so the assertion that carries the
    // case is x = 5, which only the second draw can reach - without
    // clearViewport it would have gone to 20..30 as well and left this blue.
    check(isGreen(image.at(5, middleRow))); // the second draw, unmoved
    check(isGreen(image.at(25, middleRow))); // the first draw, still there
    check(isBlue(image.at(35, middleRow))); // and neither reached past 30
};

// The documented refusal, and the reason it is a refusal rather than a clamp: a
// viewport hanging ten pixels off the right edge is ignored outright, so the
// draw lands where the default puts it. Clamped instead, it would have drawn -
// squashed into the clamped rectangle, at a scale nobody asked for, looking
// exactly like a bug in the caller's own maths.
//
// This is eacp's decision rather than something a backend forces: with the
// bounds check removed, Metal took the out-of-target viewport without
// complaint and drew through it. So there is nothing but this case keeping the
// two backends from quietly diverging on what such a rect means.
auto tViewportOutsideTheTargetIsIgnored =
    test("Viewport/outsideTheTargetIsIgnored") = []
{
    if (!Device::shared().isValid())
        return;

    const auto image = renderSteps(
        {{.firstVertex = leftHalfOfClipSpace, .viewport = {30.f, 0.f, 20.f, 40.f}}});

    if (!image.isValid())
        return;

    check(isGreen(image.at(5, middleRow)));
    check(isGreen(image.at(19, middleRow)));
    check(isBlue(image.at(25, middleRow)));
};

// An empty rect is the other half of that rule, and the one a caller reaches by
// accident - a pane collapsed to zero width, a layout before its first
// measurement. It is ignored too, rather than being handed to a backend that
// would either reject it or rasterize nothing at all for the rest of the pass.
auto tEmptyViewportIsIgnored = test("Viewport/emptyIsIgnored") = []
{
    if (!Device::shared().isValid())
        return;

    const auto image = renderSteps(
        {{.firstVertex = leftHalfOfClipSpace, .viewport = {10.f, 10.f, 0.f, 20.f}}});

    if (!image.isValid())
        return;

    check(isGreen(image.at(5, middleRow)));
    check(isBlue(image.at(25, middleRow)));
};

// near/far, which share the call and are otherwise untested by anything above:
// both halves draw a red quad at the near plane and then a green one a quarter
// of the way back, with the default less-equal test between them. The only
// difference is the depth range the red one was drawn through.
//
// At the default [0, 1] the red quad writes 0 and the green one at 0.25 fails
// behind it. Pushed to [0.5, 1] the same red quad writes 0.5 instead - its
// geometry unchanged, its depth remapped - and the green quad now passes in
// front of it. Which is the whole use for the parameter: putting a layer
// behind, or in front of, something it does not otherwise sort against.
auto tDepthRangeRemapsWhatIsWritten = test("Viewport/depthRangeRemapsWrites") = []
{
    if (!Device::shared().isValid())
        return;

    const auto defaultRange = renderSteps(
        {
            {.firstVertex = atTheNearPlane},
            {.firstVertex = aQuarterBack},
        },
        true);

    const auto pushedBack = renderSteps(
        {
            {.firstVertex = atTheNearPlane,
             .viewport = {0.f, 0.f, 40.f, 40.f},
             .nearDepth = 0.5f},
            {.firstVertex = aQuarterBack, .clearViewport = true},
        },
        true);

    if (!defaultRange.isValid() || !pushedBack.isValid())
        return;

    check(isRed(defaultRange.at(20, middleRow)));
    check(isGreen(pushedBack.at(20, middleRow)));
};
