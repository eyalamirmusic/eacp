#include "Common.h"

// The geometry deliberately covers only half of clip space: a viewport moves it
// where a scissor at the same rectangle would delete it, which is a difference
// no implementation can fake.

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

// The first quad covers x from -1 to 0 in clip space and nothing else; the
// other two fill clip space at two depths, for the near/far range.
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

// `viewport` is applied when the rect is non-empty; neither field set means the
// default.
struct Step
{
    int firstVertex = 0;
    int vertexCount = 6;

    Graphics::Rect viewport = {};
    float nearDepth = 0.f;
    float farDepth = 1.f;
    bool clearViewport = false;
};

// 40 so a half is 20 pixels and the quarters land on 10, 20 and 30: every
// assertion is a whole number of pixels rather than a rounding argument.
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
        // Set here, not in the constructor body: the pipeline is built in the
        // initialiser list and has to agree with the target. Single-sampled
        // because MSAA would feather the boundary a moved quad lands on.
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

constexpr auto middleRow = 20;
} // namespace

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

// A scissor at the same rectangle would have produced an empty image, since the
// geometry is entirely on the other side. Only a viewport puts green at x = 25.
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

// A viewport rect is y-down, like Graphics::Rect and setScissorRect: a backend
// taking y from the bottom would put the geometry at the other end.
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

// The viewport is state that stays set until clearViewport changes it back.
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

    // x = 5 carries the case: only the second draw reaches it, and without
    // clearViewport it would have gone to 20..30 as well and left this blue.
    check(isGreen(image.at(5, middleRow))); // the second draw, unmoved
    check(isGreen(image.at(25, middleRow))); // the first draw, still there
    check(isBlue(image.at(35, middleRow))); // and neither reached past 30
};

// Ignored rather than clamped: a clamped rect would draw squashed, at a scale
// nobody asked for. eacp's decision, not the backend's - Metal took an
// out-of-target viewport without complaint and drew through it.
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

// The rect a caller reaches by accident: a pane collapsed to zero width, or a
// layout before its first measurement.
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

// The red quad at the near plane writes 0 through the default range and 0.5
// through [0.5, 1], so the green quad a quarter back fails behind it in the
// first and passes in front of it in the second.
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
