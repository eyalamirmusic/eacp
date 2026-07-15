#include "Common.h"

#include <cmath>

// Drives View::renderToImage over a GPUView: the off-screen Frame renders the
// view's Metal content into an app-owned texture, GPUView reads it back, and the
// compositor folds it into the snapshot. Unlike the smoke tests -- which can
// only build resources because a real draw needs a frame + drawable -- these
// exercise an actual GPU render end to end and check the resulting pixels.
//
// Metal-only: GPU snapshot read-back is wired on the Apple backend; the Windows
// renderToImage is still a stub (see View-Windows.cpp).

using namespace nano;
using namespace eacp;
using namespace eacp::GPU;

namespace
{
bool near(const Graphics::Color& c, int r, int g, int b, int tolerance = 2)
{
    auto within = [&](float channel, int target)
    { return std::abs((int) std::lround(channel * 255.f) - target) <= tolerance; };

    return within(c.r, r) && within(c.g, g) && within(c.b, b);
}

// Clears the whole view to a fixed colour and draws nothing else.
struct ClearView final : GPUView
{
    explicit ClearView(const Graphics::Color& colorToUse)
        : clearColor(colorToUse)
    {
    }

    void render(Frame& frame) override { auto pass = frame.beginPass({clearColor}); }

    Graphics::Color clearColor;
};

const char* fillShader = R"(
#include <metal_stdlib>
using namespace metal;

struct VertexIn { float2 position [[attribute(0)]]; };

vertex float4 vertexMain(VertexIn in [[stage_in]])
{
    return float4(in.position, 0.0, 1.0);
}

fragment float4 fragmentMain() { return float4(0.0, 1.0, 0.0, 1.0); }
)";

// Draws a single oversized triangle that covers the whole viewport in green,
// over a red clear -- so a captured green image proves the pipeline draw ran
// into the off-screen target, not just the clear.
struct FillView final : GPUView
{
    FillView()
        : vertexBuffer(Device::shared().makeBuffer(fullScreenTriangle))
        , library(
              Device::shared().makeShaderLibrary(ShaderSource::msl(fillShader)
                                                     .withVertex("vertexMain")
                                                     .withFragment("fragmentMain")))
        , pipeline(makePipeline())
    {
    }

    RenderPipeline makePipeline()
    {
        auto descriptor = RenderPipelineDescriptor {};
        descriptor.library = &library;
        descriptor.sampleCount = sampleCount();
        descriptor.vertexLayout.attribute(VertexFormat::Float2, 0);
        descriptor.vertexLayout.stride = sizeof(float) * 2;

        return Device::shared().makeRenderPipeline(descriptor);
    }

    void render(Frame& frame) override
    {
        auto pass = frame.beginPass({Graphics::Color {1.f, 0.f, 0.f, 1.f}});
        pass.setPipeline(pipeline);
        pass.setVertexBuffer(vertexBuffer);
        pass.draw(3);
    }

    static constexpr float fullScreenTriangle[] = {-1.f, -1.f, 3.f, -1.f, -1.f, 3.f};

    Buffer vertexBuffer;
    ShaderLibrary library;
    RenderPipeline pipeline;
};
} // namespace

// The off-screen clear resolves and reads back as its exact colour, sized to the
// view's bounds at the requested scale.
auto tCapturesClearColor = test("GPUSnapshot/capturesClearColor") = []
{
    if (!Device::shared().isValid())
        return;

    auto view = ClearView {{0.f, 0.f, 1.f, 1.f}};
    view.setBounds({0.f, 0.f, 32.f, 24.f});

    auto image = view.renderToImage(2.f);

    check(image.isValid());
    check(image.width() == 64);
    check(image.height() == 48);
    check(near(image.at(32, 24), 0, 0, 255)); // blue, everywhere
    check(near(image.at(1, 1), 0, 0, 255));
};

// An actual pipeline draw runs through the off-screen frame: a full-viewport
// green triangle over a red clear reads back green.
auto tCapturesDrawnGeometry = test("GPUSnapshot/capturesDrawnGeometry") = []
{
    if (!Device::shared().isValid())
        return;

    auto view = FillView {};
    check(view.pipeline.isValid());

    view.setBounds({0.f, 0.f, 40.f, 40.f});

    auto image = view.renderToImage(1.f);

    check(image.isValid());
    check(near(image.at(20, 20), 0, 255, 0)); // green fill, not the red clear
    check(near(image.at(3, 3), 0, 255, 0));
};
