#import <CoreVideo/CoreVideo.h>

#include "Common.h"
#include <eacp/Core/ObjC/AutoReleasePool.h>

using namespace nano;
using namespace eacp;
using namespace eacp::GPU;

// Device::wrapPixelBuffer maps an IOSurface-backed CVPixelBuffer straight into a
// sampleable texture with no copy — the camera/video display path. The buffer is
// created Metal-compatible and IOSurface-backed, the same shape an AVFoundation
// capture frame carries.
//
// Both backends reach this, by different routes: Metal maps the buffer through a
// CVMetalTextureCache, Vulkan makes the same IOSurface the backing store of a
// VkImage via VK_EXT_metal_objects. Self-skips on a host with no GPU device
// (some headless CI VMs), like the other GPU tests.

namespace
{
// CVPixelBuffer 32BGRA orders bytes B, G, R, A, so a little-endian word reads
// 0xAARRGGBB. Written straight into the buffer's base address, which is the
// whole point: nothing here ever calls Texture::update.
constexpr std::uint32_t opaqueRed = 0xffff0000;
constexpr std::uint32_t opaqueGreen = 0xff00ff00;

struct QuadVertex
{
    float position[2];
    float uv[2];
};
} // namespace

EACP_SHADER_VALUE(QuadVertex, Float2)

namespace
{
constexpr QuadVertex fullViewportQuad[] = {
    {{-1.f, -1.f}, {0.f, 1.f}},
    {{1.f, -1.f}, {1.f, 1.f}},
    {{-1.f, 1.f}, {0.f, 0.f}},
    {{1.f, -1.f}, {1.f, 1.f}},
    {{1.f, 1.f}, {1.f, 0.f}},
    {{-1.f, 1.f}, {0.f, 0.f}},
};

struct WrapShader final : ShaderProgram
{
    WrapShader() { compile(); }

    void define() override
    {
        auto position = vertexInput(&QuadVertex::position);
        auto uv = vertexInput(&QuadVertex::uv);

        setPosition(float4(position, 0.f, 1.f));
        setFragment(sample(image, varying(uv)));
    }

    Uniform<Texture2D> image;

    EACP_SHADER(image)
};

struct WrapView final : GPUView
{
    explicit WrapView(Texture& textureToShow)
    {
        setSampleCount(1);

        shader.setVertices(fullViewportQuad, 6);
        shader.image = textureToShow;
        shader.prepare(sampleCount());
    }

    void render(Frame& frame) override
    {
        auto pass = frame.beginPass({{0.f, 0.f, 0.f, 1.f}});
        pass.draw(shader);
    }

    WrapShader shader;
};

CVPixelBufferRef makeSharedPixelBuffer(int width, int height)
{
    NSDictionary* attributes = @{
        (id) kCVPixelBufferMetalCompatibilityKey : @YES,
        (id) kCVPixelBufferIOSurfacePropertiesKey : @ {}
    };

    CVPixelBufferRef pixelBuffer = nullptr;
    auto status = CVPixelBufferCreate(kCFAllocatorDefault,
                                      (size_t) width,
                                      (size_t) height,
                                      kCVPixelFormatType_32BGRA,
                                      (__bridge CFDictionaryRef) attributes,
                                      &pixelBuffer);

    return status == kCVReturnSuccess ? pixelBuffer : nullptr;
}

// Paints the buffer's own memory, respecting its row stride. Deliberately not a
// GPU upload: the test is that the wrapped texture is looking at these bytes.
void fillPixelBuffer(CVPixelBufferRef pixelBuffer, std::uint32_t color)
{
    CVPixelBufferLockBaseAddress(pixelBuffer, 0);

    auto* base = (std::uint8_t*) CVPixelBufferGetBaseAddress(pixelBuffer);
    auto stride = CVPixelBufferGetBytesPerRow(pixelBuffer);
    auto width = CVPixelBufferGetWidth(pixelBuffer);
    auto height = CVPixelBufferGetHeight(pixelBuffer);

    for (auto y = (size_t) 0; y < height; ++y)
    {
        auto* row = (std::uint32_t*) (base + y * stride);

        for (auto x = (size_t) 0; x < width; ++x)
            row[x] = color;
    }

    CVPixelBufferUnlockBaseAddress(pixelBuffer, 0);
}

Graphics::Image drawWrapped(Texture& texture)
{
    auto view = WrapView {texture};
    view.setBounds({0.f, 0.f, 8.f, 8.f});

    return view.renderToImage(1.f);
}

// Clears to a fixed colour and nothing else: enough to tell whether the render
// landed in the buffer handed to renderNativeContentToTarget.
struct ClearView final : GPUView
{
    explicit ClearView(const Graphics::Color& colorToUse)
        : clearColor(colorToUse)
    {
        setSampleCount(1);
    }

    void render(Frame& frame) override
    {
        auto pass = frame.beginPass({clearColor});
    }

    Graphics::Color clearColor;
};

// The buffer's own memory, read back through its base address.
std::uint32_t pixelBufferTexel(CVPixelBufferRef pixelBuffer, int x, int y)
{
    CVPixelBufferLockBaseAddress(pixelBuffer, kCVPixelBufferLock_ReadOnly);

    auto* base = (const std::uint8_t*) CVPixelBufferGetBaseAddress(pixelBuffer);
    auto stride = CVPixelBufferGetBytesPerRow(pixelBuffer);
    auto texel = *(const std::uint32_t*) (base + (size_t) y * stride
                                          + (size_t) x * sizeof(std::uint32_t));

    CVPixelBufferUnlockBaseAddress(pixelBuffer, kCVPixelBufferLock_ReadOnly);

    return texel;
}
} // namespace

auto tWrapsPixelBuffer = test("GPU/wrapsPixelBuffer") = []
{
    ObjC::AutoReleasePool pool;

    auto& device = Device::shared();

    if (!device.isValid())
        return;

    auto* pixelBuffer = makeSharedPixelBuffer(4, 4);
    check(pixelBuffer != nullptr);

    if (pixelBuffer == nullptr)
        return;

    auto texture = device.wrapPixelBuffer(pixelBuffer);
    check(texture.isValid());
    check(texture.width() == 4);
    check(texture.height() == 4);

    CVPixelBufferRelease(pixelBuffer);
};

// The wrap shares memory rather than snapshotting it: the buffer is painted
// *after* it was wrapped, and again between two draws, with no Texture::update
// in between. A backend that copied on wrap would draw black, then stay red.
auto tWrappedBufferIsZeroCopy = test("GPU/wrappedPixelBufferSharesMemory") = []
{
    ObjC::AutoReleasePool pool;

    auto& device = Device::shared();

    if (!device.isValid())
        return;

    auto* pixelBuffer = makeSharedPixelBuffer(4, 4);

    if (pixelBuffer == nullptr)
        return;

    auto texture = device.wrapPixelBuffer(pixelBuffer);
    check(texture.isValid());

    if (!texture.isValid())
    {
        CVPixelBufferRelease(pixelBuffer);
        return;
    }

    fillPixelBuffer(pixelBuffer, opaqueRed);

    auto first = drawWrapped(texture);
    check(first.isValid());
    check(first.at(4, 4).r > 0.9f);
    check(first.at(4, 4).g < 0.1f);

    fillPixelBuffer(pixelBuffer, opaqueGreen);

    auto second = drawWrapped(texture);
    check(second.isValid());
    check(second.at(4, 4).g > 0.9f);
    check(second.at(4, 4).r < 0.1f);

    CVPixelBufferRelease(pixelBuffer);
};

// The write direction of the same trick: renderNativeContentToTarget renders
// the view straight into an IOSurface-backed CVPixelBuffer, which is the
// real-time tier VideoRecorder uses instead of snapshotting and copying. Checked
// by reading the buffer's own memory afterwards — nothing copies it there.
auto tRendersIntoPixelBuffer = test("GPU/rendersIntoPixelBufferTarget") = []
{
    ObjC::AutoReleasePool pool;

    auto& device = Device::shared();

    if (!device.isValid())
        return;

    auto* pixelBuffer = makeSharedPixelBuffer(16, 16);

    if (pixelBuffer == nullptr)
        return;

    auto view = ClearView {{0.f, 0.f, 1.f, 1.f}};
    view.setBounds({0.f, 0.f, 16.f, 16.f});

    // Through the base class, which is how AppleEncoder reaches it: GPUView
    // overrides it privately.
    auto& capturable = static_cast<Graphics::View&>(view);
    check(capturable.renderNativeContentToTarget(pixelBuffer, 1.f));

    // Opaque blue, as BGRA bytes: B=255, G=0, R=0, A=255.
    check(pixelBufferTexel(pixelBuffer, 8, 8) == 0xff0000ff);

    CVPixelBufferRelease(pixelBuffer);
};
