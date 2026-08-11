#include "Common.h"

#include <cmath>

using namespace nano;
using namespace eacp;
using namespace eacp::GPU;

namespace
{
constexpr auto insideRect = 2;
constexpr auto lastColumnInside = 19;
constexpr auto firstColumnOutside = 20;
constexpr auto wellOutside = 38;
constexpr auto midRow = 20;

bool isGreen(const Graphics::Color& c)
{
    return c.g > 0.5f && c.r < 0.5f;
}

bool isRed(const Graphics::Color& c)
{
    return c.r > 0.5f && c.g < 0.5f;
}

const char* mslShader = R"(
#include <metal_stdlib>
using namespace metal;

struct VertexIn { float2 position [[attribute(0)]]; };

vertex float4 vertexMain(VertexIn in [[stage_in]])
{
    return float4(in.position, 0.0, 1.0);
}

fragment float4 fragmentMain() { return float4(0.0, 1.0, 0.0, 1.0); }
)";

const char* hlslShader = R"(
struct VertexIn { float2 position : TEXCOORD0; };
struct VertexOut { float4 position : SV_Position; };

VertexOut vertexMain(VertexIn input)
{
    VertexOut o;
    o.position = float4(input.position, 0.0, 1.0);
    return o;
}

float4 fragmentMain(VertexOut input) : SV_Target { return float4(0.0, 1.0, 0.0, 1.0); }
)";

ShaderSource shaderSource()
{
    return Platform::isWindows() ? ShaderSource::hlsl(hlslShader)
                                 : ShaderSource::msl(mslShader);
}

// `scissor` is in render-target pixels.
struct ScissorView final : GPUView
{
    ScissorView()
        : library(Device::shared().makeShaderLibrary(
              shaderSource().withVertex("vertexMain").withFragment("fragmentMain")))
        , vertexBuffer(Device::shared().makeBuffer(fullScreenTriangle))
        , pipeline(makePipeline())
    {
    }

    RenderPipeline makePipeline()
    {
        // MSAA would feather the scissor boundary. Set here, not in the
        // constructor body: the pipeline is built in the initialiser list, and
        // D3D12 answers a sample-count mismatch by silently dropping the draw.
        setSampleCount(1);

        auto descriptor = RenderPipelineDescriptor {};
        descriptor.library = &library;
        descriptor.sampleCount = sampleCount();
        descriptor.vertexLayout.attribute(VertexFormat::Float2, 0);
        descriptor.vertexLayout.stride = sizeof(float) * 2;

        return Device::shared().makeRenderPipeline(descriptor);
    }

    void render(Frame& frame) override
    {
        auto pass = frame.beginPass({{1.f, 0.f, 0.f, 1.f}});

        if (useScissor)
            pass.setScissorRect(scissor);

        if (clearScissorBeforeDraw)
            pass.clearScissorRect();

        pass.setPipeline(pipeline);
        pass.setVertexBuffer(vertexBuffer);
        pass.draw(3);
    }

    static constexpr float fullScreenTriangle[] = {-1.f, -1.f, 3.f, -1.f, -1.f, 3.f};

    bool useScissor = false;
    bool clearScissorBeforeDraw = false;
    Graphics::Rect scissor;

    ShaderLibrary library;
    Buffer vertexBuffer;
    RenderPipeline pipeline;
};

// Assertions by area stay correct whichever way up the read-back arrives.
int countGreen(const Graphics::Image& image)
{
    auto total = 0;

    for (auto y = 0; y < image.height(); ++y)
        for (auto x = 0; x < image.width(); ++x)
            if (isGreen(image.at(x, y)))
                ++total;

    return total;
}
} // namespace

auto tNoScissorFillsTarget = test("Scissor/withoutScissorFillsWholeTarget") = []
{
    if (!Device::shared().isValid())
        return;

    auto view = ScissorView {};

    if (!view.pipeline.isValid())
        return;

    view.setBounds({0.f, 0.f, 40.f, 40.f});

    auto image = view.renderToImage(1.f);

    check(image.isValid());
    check(countGreen(image) == image.width() * image.height());
};

// The x axis carries the assertion: neither backend mirrors horizontally.
auto tClipsToRect = test("Scissor/clipsDrawingToRect") = []
{
    if (!Device::shared().isValid())
        return;

    auto view = ScissorView {};

    if (!view.pipeline.isValid())
        return;

    view.setBounds({0.f, 0.f, 40.f, 40.f});
    view.useScissor = true;
    view.scissor = {0.f, 0.f, 20.f, 40.f}; // left half, in pixels at scale 1

    auto image = view.renderToImage(1.f);

    check(image.isValid());
    check(image.width() == 40);

    check(isGreen(image.at(insideRect, midRow)));
    check(isGreen(image.at(lastColumnInside, midRow)));
    check(isRed(image.at(firstColumnOutside, midRow)));
    check(isRed(image.at(wellOutside, midRow)));

    check(countGreen(image) == 20 * 40);
};

auto tClipsVertically = test("Scissor/clipsOnTheVerticalAxis") = []
{
    if (!Device::shared().isValid())
        return;

    auto view = ScissorView {};

    if (!view.pipeline.isValid())
        return;

    view.setBounds({0.f, 0.f, 40.f, 40.f});
    view.useScissor = true;
    view.scissor = {0.f, 10.f, 40.f, 20.f};

    auto image = view.renderToImage(1.f);

    check(image.isValid());
    check(countGreen(image) == 40 * 20);
};

// Metal aborts under API validation on a scissor that leaves the render target,
// so reaching the checks at all is most of the result here.
auto tClampsToTarget = test("Scissor/clampsRectToRenderTarget") = []
{
    if (!Device::shared().isValid())
        return;

    auto view = ScissorView {};

    if (!view.pipeline.isValid())
        return;

    view.setBounds({0.f, 0.f, 40.f, 40.f});
    view.useScissor = true;

    view.scissor = {-1000.f, -1000.f, 5000.f, 5000.f};

    auto image = view.renderToImage(1.f);

    check(image.isValid());
    check(countGreen(image) == image.width() * image.height());
};

// Must discard every fragment rather than clamp up to something visible.
auto tEmptyRectDiscardsEverything = test("Scissor/emptyRectDrawsNothing") = []
{
    if (!Device::shared().isValid())
        return;

    auto view = ScissorView {};

    if (!view.pipeline.isValid())
        return;

    view.setBounds({0.f, 0.f, 40.f, 40.f});
    view.useScissor = true;
    view.scissor = {10.f, 10.f, 0.f, 0.f};

    auto image = view.renderToImage(1.f);

    check(image.isValid());
    check(countGreen(image) == 0);
};

// Clamps to empty, not back into view.
auto tFullyOutsideDrawsNothing = test("Scissor/fullyOutsideRectDrawsNothing") = []
{
    if (!Device::shared().isValid())
        return;

    auto view = ScissorView {};

    if (!view.pipeline.isValid())
        return;

    view.setBounds({0.f, 0.f, 40.f, 40.f});
    view.useScissor = true;
    view.scissor = {200.f, 200.f, 50.f, 50.f};

    auto image = view.renderToImage(1.f);

    check(image.isValid());
    check(countGreen(image) == 0);
};

auto tClearRestoresFullTarget = test("Scissor/clearRestoresFullTarget") = []
{
    if (!Device::shared().isValid())
        return;

    auto view = ScissorView {};

    if (!view.pipeline.isValid())
        return;

    view.setBounds({0.f, 0.f, 40.f, 40.f});
    view.useScissor = true;
    view.scissor = {0.f, 0.f, 8.f, 8.f};
    view.clearScissorBeforeDraw = true;

    auto image = view.renderToImage(1.f);

    check(image.isValid());
    check(countGreen(image) == image.width() * image.height());
};

// Pixels, not logical points, so the same rect covers half as much of a 2x
// target. Catches anyone folding the backing scale into the backend.
auto tRectIsInPixels = test("Scissor/rectIsInRenderTargetPixels") = []
{
    if (!Device::shared().isValid())
        return;

    auto view = ScissorView {};

    if (!view.pipeline.isValid())
        return;

    view.setBounds({0.f, 0.f, 40.f, 40.f});
    view.useScissor = true;
    view.scissor = {0.f, 0.f, 20.f, 80.f};

    auto image = view.renderToImage(2.f); // 80x80 pixels

    check(image.isValid());
    check(image.width() == 80);
    check(countGreen(image) == 20 * 80);
};
