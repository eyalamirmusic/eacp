#include "Common.h"

// Every case is a pair differing in one field, because a field ignored outright
// makes both halves agree. Worth running on both backends: whether Metal and
// D3D12 call the same triangle front-facing cannot be checked on one machine.

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

// A third colour, so a culled triangle and an occluded quad do not read as
// each other.
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

// The triangle is listed both ways round, the first ordering clockwise *on the
// image* since both APIs decide facing after the viewport transform. The quads
// are deliberately wound the other way, to detect a cull mode leaking onto them.
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

// A case is a list of these, because half of what is under test is what a
// second draw sees of what the first left in the depth buffer.
struct Step
{
    int firstVertex = 0;
    int vertexCount = 3;

    CullMode cullMode = CullMode::None;
    Winding frontFace = Winding::Clockwise;

    DepthCompare depthCompare = DepthCompare::LessEqual;
    bool depthWrite = true;
};

// A pipeline per step inside render(), since the cases vary state the pipeline
// bakes in. Both backends allow one created and dropped inside a frame.
struct StateView final : GPUView
{
    StateView()
        : library(Device::shared().makeShaderLibrary(
              shaderSource().withVertex("vertexMain").withFragment("fragmentMain")))
        , vertexBuffer(Device::shared().makeBuffer(geometry))
    {
        // MSAA would feather the culled edges and make the assertions
        // approximate.
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

            // Every pipeline in a pass has to agree with the view about the
            // depth attachment; Metal rejects one that does not.
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

// Where every triangle and quad in this file overlaps.
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

// The baseline: a later case finding one ordering missing found culling, not a
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

// With the default Clockwise front face, the triangle wound clockwise on the
// image survives back-face culling.
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

// D3D12 spells frontFace as a BOOL on the rasterizer state and Metal as a
// separate encoder call, so it is easy for one to keep its default.
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

// Culling is encoder state on Metal and pipeline state on D3D12: the mode stays
// set unless setPipeline applies it every time.
auto tCullingDoesNotLeakToTheNextPipeline =
    test("PipelineState/cullingDoesNotLeak") = []
{
    if (!Device::shared().isValid())
        return;

    const auto image = renderSteps({
        {.firstVertex = clockwiseTriangle, .cullMode = CullMode::Back},

        // Wound the other way: precisely what the pipeline before it culled.
        {.firstVertex = farQuad, .vertexCount = 6, .cullMode = CullMode::None},
    });

    if (!image.isValid())
        return;

    check(isGreen(centreOf(image)));
    check(isGreen(image.at(2, 2)));
};

// Both orders, because one of them comes out red with no depth buffer at all.
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

    // The order that says something: with no depth test this reads green.
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

// Both halves draw near then far under one test: only whether the near quad
// recorded its depth differs, and unrecorded the buffer is still at its 1.0.
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

// Less passes for the nearer quad and Greater does not, so a backend mapping
// the enum wrongly - or keeping less-equal - fails the second half.
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

// Both ends of the enum on identical draws, which an off-by-one in the switch
// would shift.
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
