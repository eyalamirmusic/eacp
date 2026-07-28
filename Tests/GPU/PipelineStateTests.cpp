#include "Common.h"

// RenderPipelineDescriptor's face culling and depth state, checked by drawing
// through them and reading the pixels back.
//
// None of these settings has a CPU-side observable. Each is a field copied into
// one of two very different structures - MTLDepthStencilDescriptor and encoder
// calls on one backend, D3D12_RASTERIZER_DESC and D3D12_DEPTH_STENCIL_DESC on
// the other - and a field mapped to the wrong constant still builds, still
// draws, and produces a different picture. So the assertions are pixels.
//
// Every case here is written as a *pair* that differs in one field: the same
// geometry drawn the same way twice, changing only the setting under test, with
// opposite expected results. That is what separates "this field works" from
// "something in this pipeline happens to draw green", which a single-outcome
// case cannot tell apart - and it is the shape that catches a field ignored
// outright, since ignoring it makes both halves agree.
//
// Everything renders off-screen through View::renderToImage, so it runs in CI
// on both backends. Which matters more here than usual: whether Metal and D3D12
// call the same triangle front-facing is a convention this code assumes and
// cannot check on one machine.

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

// The clear colour, and so "nothing was drawn here". A third colour rather than
// one of the two the geometry uses, so a culled triangle and an occluded quad
// read as themselves instead of as each other.
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

// Vertices 0..2 and 3..5 are one triangle listed both ways round; 6..11 and
// 12..17 are two full-target quads at different depths.
//
// The triangle's first ordering is clockwise *on the image*: in clip space its
// vertices run top-centre, bottom-right, bottom-left, and both APIs decide
// facing after the viewport transform - where y increases downwards - so that
// traversal appears clockwise on the rendered pixels. Which is the whole
// question this file exists to settle, and the reason the cases below assert an
// absolute answer rather than only that the two orderings differ: if the two
// backends disagreed about it, a relative assertion would pass on both.
//
// The quads run bottom-left, bottom-right, top-right - counter-clockwise on the
// image, the other way from the triangle. That is not incidental: it is what
// lets a quad detect a cull mode leaking off the triangle's pipeline onto the
// draw after it.
constexpr float nearDepth = 0.25f;
constexpr float farDepth = 0.75f;

// clang-format off
constexpr Vertex geometry[] = {
    green( 0.f,  0.9f, 0.f), green( 0.9f, -0.9f, 0.f), green(-0.9f, -0.9f, 0.f),
    green( 0.f,  0.9f, 0.f), green(-0.9f, -0.9f, 0.f), green( 0.9f, -0.9f, 0.f),

    red(-1.f, -1.f, nearDepth), red( 1.f, -1.f, nearDepth), red( 1.f, 1.f, nearDepth),
    red(-1.f, -1.f, nearDepth), red( 1.f,  1.f, nearDepth), red(-1.f, 1.f, nearDepth),

    green(-1.f, -1.f, farDepth), green( 1.f, -1.f, farDepth), green( 1.f, 1.f, farDepth),
    green(-1.f, -1.f, farDepth), green( 1.f,  1.f, farDepth), green(-1.f, 1.f, farDepth),
};
// clang-format on

constexpr auto clockwiseTriangle = 0;
constexpr auto counterClockwiseTriangle = 3;
constexpr auto nearQuad = 6;
constexpr auto farQuad = 12;

// One draw, and the pipeline state it is drawn with. A case is a list of these,
// because half of what is under test is what a *second* draw sees of what the
// first one left in the depth buffer.
struct Step
{
    int firstVertex = 0;
    int vertexCount = 3;

    CullMode cullMode = CullMode::None;
    Winding frontFace = Winding::Clockwise;

    DepthCompare depthCompare = DepthCompare::LessEqual;
    bool depthWrite = true;
};

// Builds a pipeline per step inside render(), rather than holding them: the
// cases vary state the pipeline bakes in, so one view shape has to cover a
// dozen different pipelines. Both backends already allow a pipeline created and
// dropped inside a frame - the D3D12 one defers its release until the command
// list that referenced it has run.
struct StateView final : GPUView
{
    StateView()
        : library(Device::shared().makeShaderLibrary(
              shaderSource().withVertex("vertexMain").withFragment("fragmentMain")))
        , vertexBuffer(Device::shared().makeBuffer(geometry))
    {
        // MSAA would feather the culled triangle's edges and average two quads
        // along the boundary, turning exact pixel assertions into approximate
        // ones for no gain here.
        setSampleCount(1);
    }

    void render(Frame& frame) override
    {
        auto pass = frame.beginPass({{0.f, 0.f, 1.f, 1.f}});

        for (const auto& step: steps)
        {
            auto descriptor = RenderPipelineDescriptor {};
            descriptor.library = &library;
            descriptor.sampleCount = sampleCount();
            descriptor.vertexLayout.attribute(VertexFormat::Float3, 0)
                .attribute(VertexFormat::Float4, sizeof(float) * 3);
            descriptor.vertexLayout.stride = sizeof(Vertex);
            descriptor.cullMode = step.cullMode;
            descriptor.frontFace = step.frontFace;

            // Whether there is a depth attachment is the view's decision and
            // every pipeline in a pass has to agree with it - a Metal pipeline
            // that disagrees is rejected outright.
            descriptor.depth = hasDepth();
            descriptor.depthCompare = step.depthCompare;
            descriptor.depthWrite = step.depthWrite;

            auto pipeline = Device::shared().makeRenderPipeline(descriptor);

            if (!pipeline.isValid())
                return;

            pass.setPipeline(pipeline);
            pass.setVertexBuffer(vertexBuffer);
            pass.draw(step.vertexCount, step.firstVertex);
        }
    }

    Vector<Step> steps;

    ShaderLibrary library;
    Buffer vertexBuffer;
};

// The centre pixel, where every triangle and quad in this file overlaps.
Graphics::Color centreOf(const Graphics::Image& image)
{
    return image.at(image.width() / 2, image.height() / 2);
}

Graphics::Image renderSteps(const Vector<Step>& steps, bool depth = false)
{
    auto view = StateView {};
    view.steps = steps;
    view.setDepth(depth);
    view.setBounds({0.f, 0.f, 40.f, 40.f});

    return view.renderToImage(1.f);
}
} // namespace

// The baseline. Both orderings of the triangle cover the centre when nothing is
// culled, so a later case finding one of them missing found culling and not a
// broken vertex buffer.
auto tNoCullingDrawsEitherWinding = test("PipelineState/noCullingDrawsEither") = []
{
    if (!Device::shared().isValid())
        return;

    const auto clockwise = renderSteps({{.firstVertex = clockwiseTriangle}});
    const auto other = renderSteps({{.firstVertex = counterClockwiseTriangle}});

    if (!clockwise.isValid())
        return;

    check(isGreen(centreOf(clockwise)));
    check(isGreen(centreOf(other)));
};

// The absolute statement, and the one that has to hold on both backends for
// anything built on culling to be portable: with the default Clockwise front
// face, a triangle wound clockwise on the image survives back-face culling and
// the same triangle listed the other way round does not.
auto tBackCullingKeepsTheFrontFace = test("PipelineState/backCullingKeepsFront") = []
{
    if (!Device::shared().isValid())
        return;

    const auto kept = renderSteps(
        {{.firstVertex = clockwiseTriangle, .cullMode = CullMode::Back}});
    const auto culled = renderSteps(
        {{.firstVertex = counterClockwiseTriangle, .cullMode = CullMode::Back}});

    if (!kept.isValid())
        return;

    check(isGreen(centreOf(kept)));
    check(isBlue(centreOf(culled)));
};

// frontFace is the other half of the pair, and the half a backend can drop
// silently: D3D12 spells it as a BOOL on the rasterizer state and Metal as a
// separate encoder call, so it is easy for one of them to keep its default
// while the other follows the descriptor. Flipping it must flip which ordering
// survives - the exact inverse of the case above, same geometry, same cull mode.
auto tFrontFaceChoosesWhichSideIsFront =
    test("PipelineState/frontFaceChoosesTheSide") = []
{
    if (!Device::shared().isValid())
        return;

    const auto culled = renderSteps({{.firstVertex = clockwiseTriangle,
                                      .cullMode = CullMode::Back,
                                      .frontFace = Winding::CounterClockwise}});
    const auto kept = renderSteps({{.firstVertex = counterClockwiseTriangle,
                                    .cullMode = CullMode::Back,
                                    .frontFace = Winding::CounterClockwise}});

    if (!culled.isValid())
        return;

    check(isBlue(centreOf(culled)));
    check(isGreen(centreOf(kept)));
};

// Front culling, which is the setting a mirror or an inside-out skybox uses,
// and which must throw away exactly what back culling keeps.
auto tFrontCullingIsTheOpposite = test("PipelineState/frontCullingIsOpposite") = []
{
    if (!Device::shared().isValid())
        return;

    const auto culled = renderSteps(
        {{.firstVertex = clockwiseTriangle, .cullMode = CullMode::Front}});
    const auto kept = renderSteps(
        {{.firstVertex = counterClockwiseTriangle, .cullMode = CullMode::Front}});

    if (!culled.isValid())
        return;

    check(isBlue(centreOf(culled)));
    check(isGreen(centreOf(kept)));
};

// Culling is encoder state on Metal and pipeline state on D3D12, and this is
// the case that difference can break: a pass that binds a culling pipeline and
// then a non-culling one leaves the cull mode set on Metal unless setPipeline
// applies it every time. The second draw covers the whole target, so if it were
// culled the centre would still be the first triangle's green.
auto tCullingDoesNotLeakToTheNextPipeline =
    test("PipelineState/cullingDoesNotLeak") = []
{
    if (!Device::shared().isValid())
        return;

    const auto image = renderSteps({
        {.firstVertex = clockwiseTriangle, .cullMode = CullMode::Back},

        // Wound the other way, so it is precisely what the pipeline before it
        // was culling.
        {.firstVertex = farQuad, .vertexCount = 6, .cullMode = CullMode::None},
    });

    if (!image.isValid())
        return;

    check(isGreen(centreOf(image)));
    check(isGreen(image.at(2, 2)));
};

// Depth, starting with the baseline: with the test running and writes on, the
// nearer quad wins whichever order the two are drawn in. Both orders, because
// one of them would come out red with no depth buffer at all and the assertion
// has to be the one that cannot.
auto tNearerGeometryWins = test("PipelineState/nearerGeometryWins") = []
{
    if (!Device::shared().isValid())
        return;

    const auto farFirst = renderSteps(
        {
            {.firstVertex = farQuad, .vertexCount = 6},
            {.firstVertex = nearQuad, .vertexCount = 6},
        },
        true);

    // The order that says something: without a depth test the last draw wins
    // and this reads green.
    const auto nearFirst = renderSteps(
        {
            {.firstVertex = nearQuad, .vertexCount = 6},
            {.firstVertex = farQuad, .vertexCount = 6},
        },
        true);

    if (!farFirst.isValid() || !nearFirst.isValid())
        return;

    check(isRed(centreOf(farFirst)));
    check(isRed(centreOf(nearFirst)));
};

// depthWrite on its own. Both halves draw the near quad first and the far quad
// second with an unchanged LessEqual test; the only difference is whether the
// near one recorded its depth. When it did not, the buffer is still at its
// cleared 1.0 and the further quad passes - which is exactly how translucent
// geometry has to behave over the top of other translucent geometry.
auto tDepthWriteOffLeavesTheBufferAlone =
    test("PipelineState/depthWriteOffLeavesBuffer") = []
{
    if (!Device::shared().isValid())
        return;

    const auto writing = renderSteps(
        {
            {.firstVertex = nearQuad, .vertexCount = 6, .depthWrite = true},
            {.firstVertex = farQuad, .vertexCount = 6},
        },
        true);

    const auto notWriting = renderSteps(
        {
            {.firstVertex = nearQuad, .vertexCount = 6, .depthWrite = false},
            {.firstVertex = farQuad, .vertexCount = 6},
        },
        true);

    if (!writing.isValid() || !notWriting.isValid())
        return;

    check(isRed(centreOf(writing)));
    check(isGreen(centreOf(notWriting)));
};

// The comparison on its own, by the same construction: the far quad goes down
// first and writes its depth, then the near quad is drawn twice with the two
// tests that disagree about it. Less passes - it is nearer - and Greater does
// not. A backend mapping the enum to the wrong constant, or ignoring it and
// keeping less-equal, fails the second half.
auto tCompareFunctionDecidesWhatPasses =
    test("PipelineState/compareFunctionDecides") = []
{
    if (!Device::shared().isValid())
        return;

    const auto less = renderSteps(
        {
            {.firstVertex = farQuad, .vertexCount = 6},
            {.firstVertex = nearQuad,
             .vertexCount = 6,
             .depthCompare = DepthCompare::Less},
        },
        true);

    const auto greater = renderSteps(
        {
            {.firstVertex = farQuad, .vertexCount = 6},
            {.firstVertex = nearQuad,
             .vertexCount = 6,
             .depthCompare = DepthCompare::Greater},
        },
        true);

    if (!less.isValid() || !greater.isValid())
        return;

    check(isRed(centreOf(less)));
    check(isGreen(centreOf(greater)));
};

// The two ends of the enum, on identical draws. The near quad goes down first
// and writes 0.25; the far quad follows with a test that either always fails or
// always passes, and nothing else about the two halves differs. Always is what
// a pass draws with when it wants the depth attachment present and the test out
// of the way, and it is the one that has to keep a fragment less-equal would
// have thrown out - so the pair pins both ends of a switch an off-by-one would
// shift.
auto tNeverAndAlwaysAreTheExtremes = test("PipelineState/neverAndAlwaysAgree") = []
{
    if (!Device::shared().isValid())
        return;

    const auto never = renderSteps(
        {
            {.firstVertex = nearQuad, .vertexCount = 6},
            {.firstVertex = farQuad,
             .vertexCount = 6,
             .depthCompare = DepthCompare::Never},
        },
        true);

    const auto always = renderSteps(
        {
            {.firstVertex = nearQuad, .vertexCount = 6},
            {.firstVertex = farQuad,
             .vertexCount = 6,
             .depthCompare = DepthCompare::Always},
        },
        true);

    if (!never.isValid() || !always.isValid())
        return;

    check(isRed(centreOf(never)));
    check(isGreen(centreOf(always)));
};
