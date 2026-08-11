#pragma once

#include "../Common.h"

namespace eacp::GPU
{
class Device;

enum class TextureFormat
{
    RGBA8Unorm,
    BGRA8Unorm,

    // Sampled as (r, 0, 0, 1).
    R8Unorm,

    // Sampled as (r, g, 0, 1); an NV12 frame's Cb/Cr plane, its luma plane
    // being an R8Unorm of twice the width and height.
    RG8Unorm,

    // For feedback passes, which quantise a value away over many frames at 8
    // bits. RGBA32Float filtering is not guaranteed - sample it Nearest.
    RGBA16Float,
    RGBA32Float
};

constexpr int bytesPerPixel(TextureFormat format)
{
    switch (format)
    {
        case TextureFormat::R8Unorm:
            return 1;
        case TextureFormat::RG8Unorm:
            return 2;
        case TextureFormat::RGBA16Float:
            return 8;
        case TextureFormat::RGBA32Float:
            return 16;
        default:
            return 4;
    }
}

constexpr bool isFloatFormat(TextureFormat format)
{
    return format == TextureFormat::RGBA16Float
           || format == TextureFormat::RGBA32Float;
}

// D3D12 only guarantees a typed UAV store for a few formats - BGRA8Unorm, the
// drawable's own, is not among them. Other formats fail to create rather than
// binding as a kernel output that does nothing.
constexpr bool supportsComputeWrite(TextureFormat format)
{
    return format == TextureFormat::RGBA8Unorm
           || format == TextureFormat::RGBA16Float
           || format == TextureFormat::RGBA32Float;
}

enum class TextureFilter
{
    Linear,
    Nearest
};

enum class TextureAddressMode
{
    Clamp,
    Repeat
};

// Belongs to the shader, not the Texture: a Windows-on-Arm driver resolves
// every sampler to heap descriptor 0, so D3D12 uses static samplers. Sampling
// is thus fixed at compile time - one slot, one way, one program each.
struct TextureSampling
{
    TextureFilter filter = TextureFilter::Nearest;
    TextureAddressMode addressMode = TextureAddressMode::Clamp;
};

// The D3D12 root signature reserves this many static samplers per texture slot,
// a sampler's register being slot * this + samplingIndex.
constexpr int samplingConfigurations = 4;

constexpr int samplingIndex(const TextureSampling& sampling)
{
    return (sampling.filter == TextureFilter::Linear ? 2 : 0)
           + (sampling.addressMode == TextureAddressMode::Repeat ? 1 : 0);
}

// Sampler state is deliberately absent: it comes from the shader, as a
// TextureSampling declared on its texture member.
struct TextureDescriptor
{
    int width = 0;
    int height = 0;
    TextureFormat format = TextureFormat::RGBA8Unorm;

    // Lets Frame::beginPass(texture) render into it, at the cost of a
    // render-target descriptor on D3D12. Create one with null `pixels`;
    // update() is not the way to fill a render target.
    bool renderTarget = false;

    // Lets ComputePass::setOutputTexture write into it, at the cost of a UAV
    // descriptor on D3D12. Only supportsComputeWrite() formats on a device
    // reporting a typed UAV store may ask; creation fails otherwise.
    bool computeWrite = false;

    // Half-size levels down to 1x1, built on the CPU from the pixels passed in,
    // so a texture created with none gets no chain. update() rebuilds them;
    // update(region, ...) refreshes level 0 only.
    bool mipmapped = false;

    // A depth buffer for passes into this texture, without which a 3D scene
    // comes out in painter's order. Costs the colour texture's size again, and
    // is cleared to the far plane every pass. Ignored without renderTarget.
    bool depth = false;
};

// A 2D texture sampled by the fragment stage. Create via Device::makeTexture
// with tightly packed pixels (bytesPerPixel each), row 0 at the top, or null
// pixels for an uninitialised one. Bind with RenderPass::setFragmentTexture.
class Texture
{
public:
    Texture(Device& device, const TextureDescriptor& descriptor, const void* pixels);

    // Zero-copy wrap of a platform pixel buffer (CVPixelBuffer on macOS), which
    // must outlive the texture. Invalid on Windows; use update() there.
    Texture(Device& device, void* nativePixelBuffer);

    int width() const;
    int height() const;
    bool isValid() const;

    bool isRenderTarget() const;
    bool isComputeWritable() const;

    // 1 unless the texture asked for a chain and got one; a format the chain
    // builder cannot average yields 1 rather than uninitialised lower levels.
    int mipLevels() const;

    // False on a render target that asked for no depth buffer, whose passes then
    // run without the depth test.
    bool hasDepth() const;

    // Reuses the GPU resource. Source rows are tightly packed unless bytesPerRow
    // gives a larger stride (0 means width * bytesPerPixel), matching the padded
    // rows capture buffers carry. A no-op on a wrapped or invalid texture.
    void update(const void* pixels, std::size_t bytesPerRow = 0);

    // region is in texels, origin top-left; pixels points at its own top-left,
    // rows packed to the *region's* width unless bytesPerRow says otherwise. An
    // empty or out-of-bounds region is a no-op, clamping it skewing the pixels.
    void update(const Graphics::Rect& region,
                const void* pixels,
                std::size_t bytesPerRow = 0);

    // No sampler handle: that comes from the sampling the shader declared.
    void* nativeTexture() const;

    // Null on Metal, where the texture is bound directly.
    void* nativeReadView() const;

    // Null on D3D12, where the depth resource lives inside nativeTexture's data.
    void* nativeDepthTexture() const;

private:
    struct Native;
    Pimpl<Native> impl;
};
} // namespace eacp::GPU
