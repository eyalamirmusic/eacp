#include "Common.h"

#include <cmath>

// RenderPass::setScissorRect / clearScissorRect, checked by rendering off-screen
// and reading the pixels back. A scissor rect is one of the few pieces of GPU
// state with no CPU-side observable at all -- nothing to query, nothing
// returned -- so the only honest test is to draw through it and look at what
// came out.
//
// Every case draws one full-viewport triangle over a contrasting clear colour,
// so a pixel is green exactly where the scissor let the fragment through and the
// clear's red everywhere else.
//
// Runs on both backends, and self-skips without a GPU device.

using namespace nano;
using namespace eacp;
using namespace eacp::GPU;

namespace
{
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

// Fills the viewport with green over a red clear, through whatever scissor the
// test installs. `scissor` is in render-target pixels; when `useScissor` is
// false the pass is left alone, which is the control case.
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
        // MSAA would feather the scissor boundary across a pixel and make the
        // edge assertions ambiguous. It has to be set here rather than in the
        // constructor body: the pipeline is built in the member initialiser
        // list, which runs first, so a body call would leave the pipeline
        // multisampled while the target is not -- a mismatch D3D12 answers by
        // dropping the draw, with no error and nothing rendered at all.
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

// Counts green pixels across the whole image, for assertions that care how much
// survived rather than exactly where -- those stay correct whichever way up the
// backend hands the read-back back.
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

// The baseline the rest of the file leans on: with no scissor set, the triangle
// covers everything. If this ever fails the other cases prove nothing.
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

// The x axis needs no orientation assumption -- neither backend mirrors
// horizontally -- so a left-half scissor pins down the geometry exactly.
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

    check(isGreen(image.at(2, 20)));   // inside
    check(isGreen(image.at(19, 20)));  // last column inside
    check(isRed(image.at(20, 20)));    // first column outside
    check(isRed(image.at(38, 20)));    // well outside

    check(countGreen(image) == 20 * 40);
};

// Clipping on the other axis, asserted by area so the test does not depend on
// which way up the read-back arrives.
auto tClipsVertically = test("Scissor/clipsOnTheVerticalAxis") = []
{
    if (!Device::shared().isValid())
        return;

    auto view = ScissorView {};

    if (!view.pipeline.isValid())
        return;

    view.setBounds({0.f, 0.f, 40.f, 40.f});
    view.useScissor = true;
    view.scissor = {0.f, 10.f, 40.f, 20.f}; // a horizontal band

    auto image = view.renderToImage(1.f);

    check(image.isValid());
    check(countGreen(image) == 40 * 20);
};

// The clamp is the reason this API is safe to call from a scrolled view. Metal
// aborts under API validation on a scissor that leaves the render target, so a
// region scrolled half off-screen would otherwise have to be clamped by every
// caller. Reaching the checks at all is most of the result here.
auto tClampsToTarget = test("Scissor/clampsRectToRenderTarget") = []
{
    if (!Device::shared().isValid())
        return;

    auto view = ScissorView {};

    if (!view.pipeline.isValid())
        return;

    view.setBounds({0.f, 0.f, 40.f, 40.f});
    view.useScissor = true;

    // Starts above and left of the target and runs well past its far corner.
    view.scissor = {-1000.f, -1000.f, 5000.f, 5000.f};

    auto image = view.renderToImage(1.f);

    check(image.isValid());
    check(countGreen(image) == image.width() * image.height());
};

// A scrolled-away pane collapses to an empty rect rather than a negative one;
// it must discard every fragment instead of clamping up to something visible.
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

// A rect entirely past the far edge clamps to empty, not back into view -- the
// difference between a pane scrolled out of sight and one that reappears.
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

// clearScissorRect puts the whole target back, so a widget tree can restore
// state after drawing a clipped child.
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

// The rect is in render-target pixels, not logical points, so the same rect
// covers half as much of a 2x target. Catches anyone "helpfully" folding the
// backing scale into the backend.
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
