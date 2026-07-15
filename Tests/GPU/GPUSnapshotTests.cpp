#include "Common.h"

#include <cmath>

// Drives View::renderToImage over a GPUView: the off-screen Frame renders the
// view's GPU content into an app-owned texture, GPUView reads it back, and the
// compositor folds it into the snapshot. Unlike the smoke tests -- which can
// only build resources because a real draw needs a frame + drawable -- these
// exercise an actual GPU render end to end and check the resulting pixels.
//
// Runs on both backends: Metal off-screen textures on Apple, D3D12 off-screen
// render targets on Windows, each self-skipping without a GPU device.

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

// Clears the whole view to a fixed colour and counts render() calls, so a test
// can prove a snapshot renders it exactly once (off-screen) rather than also
// presenting a live frame through paint().
struct ClearView final : GPUView
{
    explicit ClearView(const Graphics::Color& colorToUse)
        : clearColor(colorToUse)
    {
    }

    void render(Frame& frame) override
    {
        ++renders;
        auto pass = frame.beginPass({clearColor});
    }

    Graphics::Color clearColor;
    int renders = 0;
};

const char* mslFillShader = R"(
#include <metal_stdlib>
using namespace metal;

struct VertexIn { float2 position [[attribute(0)]]; };

vertex float4 vertexMain(VertexIn in [[stage_in]])
{
    return float4(in.position, 0.0, 1.0);
}

fragment float4 fragmentMain() { return float4(0.0, 1.0, 0.0, 1.0); }
)";

const char* hlslFillShader = R"(
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

// A premultiplied translucent red (rgb already scaled by alpha 0.5): the shape a
// normally alpha-blended fragment leaves in the target. Written verbatim under
// the default (no-blend) pipeline, so the read-back must un-premultiply it back
// to straight (0.5, 0, 0, 0.5) rather than leave it dark.
const char* mslPremulShader = R"(
#include <metal_stdlib>
using namespace metal;

struct VertexIn { float2 position [[attribute(0)]]; };

vertex float4 vertexMain(VertexIn in [[stage_in]])
{
    return float4(in.position, 0.0, 1.0);
}

fragment float4 fragmentMain() { return float4(0.25, 0.0, 0.0, 0.5); }
)";

const char* hlslPremulShader = R"(
struct VertexIn { float2 position : TEXCOORD0; };
struct VertexOut { float4 position : SV_Position; };

VertexOut vertexMain(VertexIn input)
{
    VertexOut o;
    o.position = float4(input.position, 0.0, 1.0);
    return o;
}

float4 fragmentMain(VertexOut input) : SV_Target { return float4(0.25, 0.0, 0.0, 0.5); }
)";

// Both branches name every string, so none is an unused-variable warning on the
// platform whose backend isn't selected.
ShaderSource fillShaderSource()
{
    return Platform::isWindows() ? ShaderSource::hlsl(hlslFillShader)
                                 : ShaderSource::msl(mslFillShader);
}

ShaderSource premulShaderSource()
{
    return Platform::isWindows() ? ShaderSource::hlsl(hlslPremulShader)
                                 : ShaderSource::msl(mslPremulShader);
}

// Draws a single oversized triangle covering the whole viewport with the given
// fragment shader, over the given clear colour.
struct TriangleView final : GPUView
{
    TriangleView(ShaderSource source, const Graphics::Color& clearToUse)
        : clearColor(clearToUse)
        , library(Device::shared().makeShaderLibrary(
              source.withVertex("vertexMain").withFragment("fragmentMain")))
        , vertexBuffer(Device::shared().makeBuffer(fullScreenTriangle))
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
        auto pass = frame.beginPass({clearColor});
        pass.setPipeline(pipeline);
        pass.setVertexBuffer(vertexBuffer);
        pass.draw(3);
    }

    static constexpr float fullScreenTriangle[] = {-1.f, -1.f, 3.f, -1.f, -1.f, 3.f};

    Graphics::Color clearColor;
    ShaderLibrary library;
    Buffer vertexBuffer;
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

    auto view = TriangleView {fillShaderSource(), {1.f, 0.f, 0.f, 1.f}};
    check(view.pipeline.isValid());

    view.setBounds({0.f, 0.f, 40.f, 40.f});

    auto image = view.renderToImage(1.f);

    check(image.isValid());
    check(near(image.at(20, 20), 0, 255, 0)); // green fill, not the red clear
    check(near(image.at(3, 3), 0, 255, 0));
};

// Translucent GPU content is composited premultiplied; the read-back
// un-premultiplies it so the Image holds straight alpha. A premultiplied
// (0.25, 0, 0, 0.5) target must come back as straight (128, 0, 0) at alpha 128,
// not the un-corrected (64, 0, 0).
auto tUnpremultipliesTranslucent =
    test("GPUSnapshot/unpremultipliesTranslucentContent") = []
{
    if (!Device::shared().isValid())
        return;

    auto view = TriangleView {premulShaderSource(), {0.f, 0.f, 0.f, 0.f}};
    check(view.pipeline.isValid());

    view.setBounds({0.f, 0.f, 40.f, 40.f});

    auto image = view.renderToImage(1.f);
    check(image.isValid());

    auto c = image.at(20, 20);
    check(near(c, 128, 0, 0, 6));
    check(std::abs((int) std::lround(c.a * 255.f) - 128) <= 6);
};

// A snapshot draws the GPU view once, off-screen: paint() must not also present
// a live frame. render() runs exactly once per renderToImage (via
// renderNativeContent), not a second time through paint()'s renderNow().
auto tSnapshotRendersOnce = test("GPUSnapshot/snapshotRendersOnce") = []
{
    if (!Device::shared().isValid())
        return;

    auto view = ClearView {{0.f, 0.f, 1.f, 1.f}};
    view.setBounds({0.f, 0.f, 16.f, 16.f});

    view.renders = 0; // discard any render from the resize above
    auto image = view.renderToImage(1.f);

    check(image.isValid());
    check(view.renders == 1);
};
