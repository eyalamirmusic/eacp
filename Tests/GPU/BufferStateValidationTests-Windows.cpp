#include "GpuBasedValidation-Windows.h"

#include <eacp/GPU/GPU.h>

#include <NanoTest/NanoTest.h>

#include <cstdint>
#include <string>

// Buffer state tracking under the D3D12 debug layer's GPU-based validation, in a
// binary of its own because validation has to be on before the device exists.

using namespace nano;
using namespace eacp;
using namespace eacp::GPU;

namespace
{
constexpr auto viewsToBuild = 4;
constexpr auto framesPerView = 2;
constexpr auto textureSize = 4;
constexpr auto bytesPerPixel = 4;
constexpr auto viewSize = 16.f;
constexpr auto snapshotScale = 1.f;

auto validationLayerEnabled = false;

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

TextureDescriptor smallTextureDescriptor()
{
    auto descriptor = TextureDescriptor {};
    descriptor.width = textureSize;
    descriptor.height = textureSize;
    return descriptor;
}

// The shape of a view that fills its vertex buffer once, outside any frame, and
// updates a texture inside every frame before drawing from that buffer.
struct TriangleDrawnAfterATextureUpdate final : GPUView
{
    TriangleDrawnAfterATextureUpdate()
        : library(
              Device::shared().makeShaderLibrary(ShaderSource::hlsl(hlslFillShader)
                                                     .withVertex("vertexMain")
                                                     .withFragment("fragmentMain")))
        , texture(
              Device::shared().makeTexture(smallTextureDescriptor(), pixels.data()))
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
        auto pass = frame.beginPass({Graphics::Color {0.f, 0.f, 0.f, 1.f}});
        texture.update(pixels.data());
        pass.setPipeline(pipeline);
        pass.setVertexBuffer(vertexBuffer);
        pass.draw(3);
    }

    static constexpr float fullScreenTriangle[] = {-1.f, -1.f, 3.f, -1.f, -1.f, 3.f};

    Vector<std::uint8_t> pixels =
        Vector<std::uint8_t>(textureSize * textureSize * bytesPerPixel);
    ShaderLibrary library;
    Texture texture;
    Buffer vertexBuffer;
    RenderPipeline pipeline;
};

winrt::com_ptr<ID3D12InfoQueue> infoQueueOrSkip()
{
    if (!validationLayerEnabled)
        return skipBecause(
                   "no D3D12 debug layer; install the Windows Graphics Tools"),
               nullptr;

    if (!Device::shared().isValid())
        return skipBecause("no device"), nullptr;

    auto queue = winrt::com_ptr<ID3D12InfoQueue>();

    if (FAILED(nativeDevice()->QueryInterface(__uuidof(ID3D12InfoQueue),
                                              queue.put_void())))
        return skipBecause("the device has no info queue"), nullptr;

    return queue;
}
} // namespace

// A buffer filled before any frame, on a recording of its own, has to be in a
// state every later draw can read it as a vertex buffer from. GPU-based
// validation on Intel Iris Xe reported every such draw while the buffer was left
// to decay out of COPY_DEST.
auto tVertexBufferFilledOutsideAFrame = test(
    "BufferStateValidation/vertexBufferFilledOutsideAFrameIsReadableInEveryFrame") =
    []
{
    auto queue = infoQueueOrSkip();

    if (!queue)
        return;

    takeErrors(*queue);

    for (auto round = 0; round < viewsToBuild; ++round)
    {
        auto view = TriangleDrawnAfterATextureUpdate {};
        view.setBounds({0.f, 0.f, viewSize, viewSize});

        for (auto frame = 0; frame < framesPerView; ++frame)
            check(view.renderToImage(snapshotScale).isValid());
    }

    waitForTheGpu();
    auto errors = takeErrors(*queue);

    check(errors.count == 0,
          std::to_string(errors.count) + " validation errors over "
              + std::to_string(viewsToBuild) + " views, first: " + errors.first);
};

int main(int argc, char* argv[])
{
    validationLayerEnabled = enableGpuBasedValidation();
    return nano::run(argc, argv);
}
