#pragma once

#include "../Common.h"

namespace eacp::GPU
{
class Device;

enum class TextureFormat
{
    RGBA8Unorm,
    BGRA8Unorm,

    // Single 8-bit channel, sampled as (r, 0, 0, 1). The natural format for
    // palette indices, masks and other one-byte-per-pixel data.
    R8Unorm
};

constexpr int bytesPerPixel(TextureFormat format)
{
    return format == TextureFormat::R8Unorm ? 1 : 4;
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

// How a shader wants one of its textures sampled.
//
// This belongs to the *shader*, not to the Texture, and that is a deliberate
// break from the obvious design. D3D12 offers two ways to give a draw its
// samplers: a descriptor table, which can vary per draw and would let the
// Texture carry its own state, or a static sampler baked into the root
// signature, which cannot. eacp used the descriptor table until a Windows-on-Arm
// driver turned out to ignore the table's offset outright and resolve every
// sampler to descriptor 0 of the heap - so every texture in the process sampled
// through whichever sampler happened to be first, and no per-texture state had
// any effect. Static samplers are unaffected, being nowhere near a heap.
//
// Since the configuration space is tiny, the root signature simply declares one
// static sampler for every (texture slot, configuration) pair and the emitter
// points each texture's sampler at the matching register. Metal has no such
// bug, but reads the same declaration so the two backends cannot drift apart.
//
// The cost is that the sampling is fixed when the shader is compiled rather
// than when a texture is bound: one shader samples one slot exactly one way.
// A renderer that must do both - Sprites::SpriteRenderer draws smoothly scaled
// camera frames and crisp pixel art through the same code - compiles one
// program per configuration and picks between them.
struct TextureSampling
{
    TextureFilter filter = TextureFilter::Nearest;
    TextureAddressMode addressMode = TextureAddressMode::Clamp;
};

// The number of distinct sampling configurations, and the index of one. The
// D3D12 root signature reserves this many static samplers per texture slot, and
// the register a texture's sampler lands on is slot * this + index; the Metal
// backend caches this many MTLSamplerStates on the Device.
constexpr int samplingConfigurations = 4;

constexpr int samplingIndex(const TextureSampling& sampling)
{
    return (sampling.filter == TextureFilter::Linear ? 2 : 0)
           + (sampling.addressMode == TextureAddressMode::Repeat ? 1 : 0);
}

// How the texture is created. Sampler state is deliberately absent: it comes
// from the shader that samples the texture, as a TextureSampling declared on
// its texture member.
struct TextureDescriptor
{
    int width = 0;
    int height = 0;
    TextureFormat format = TextureFormat::RGBA8Unorm;
};

// A 2D texture sampled by the fragment stage (MTLTexture on Metal, a D3D12
// resource with its SRV descriptor on Windows). Create via
// Device::makeTexture with tightly packed pixels (the format's
// bytesPerPixel each), row 0 at the top, or null pixels for an
// uninitialised texture. Bind with RenderPass::setFragmentTexture.
class Texture
{
public:
    Texture(Device& device, const TextureDescriptor& descriptor, const void* pixels);

    // Wraps an existing platform pixel buffer (a CVPixelBuffer on macOS) as a
    // sampleable texture without copying its pixels — the zero-copy path for
    // camera and video frames. The buffer must outlive the texture. Yields an
    // invalid texture on backends without zero-copy support (Windows for now),
    // where update() is the per-frame upload path instead.
    Texture(Device& device, void* nativePixelBuffer);

    int width() const;
    int height() const;
    bool isValid() const;

    // Re-uploads pixels into a texture created by Device::makeTexture, reusing
    // the GPU resource instead of allocating a new one — the per-frame path for
    // video and camera streams. Source rows are tightly packed unless
    // bytesPerRow gives a larger stride (0 means width * the format's
    // bytesPerPixel), matching the padded rows capture buffers often carry. A
    // no-op on a wrapped or invalid texture, or when pixels is null.
    void update(const void* pixels, std::size_t bytesPerRow = 0);

    // Re-uploads one sub-rectangle, leaving the rest of the texture untouched.
    //
    // The reason this exists: a glyph atlas grows one glyph at a time, and
    // whole-texture update() makes each new glyph cost an upload of the entire
    // atlas — megabytes to move a few hundred bytes. Here the transfer is the
    // size of the region.
    //
    // region is in texels with the origin at the top-left. pixels points at the
    // region's own top-left, and its rows are tightly packed to the *region's*
    // width unless bytesPerRow gives a larger stride — so a glyph can be
    // uploaded straight out of a larger rasterization buffer by passing that
    // buffer's stride.
    //
    // A region that is empty, or that is not wholly inside the texture, is a
    // no-op. Deliberately not clamped: a clamped region would keep consuming
    // source rows at the original width and silently upload skewed pixels,
    // which is far harder to spot than nothing appearing.
    void update(const Graphics::Rect& region,
                const void* pixels,
                std::size_t bytesPerRow = 0);

    // Opaque native handles for cross-translation-unit use by the render pass.
    // There is no sampler handle: the render pass gets that from the sampling
    // the shader declared, not from the texture.
    void* nativeTexture() const;

    // The read view the fragment stage binds on D3D12 (the same handle as
    // nativeTexture). Null on Metal, where the texture is bound directly.
    void* nativeReadView() const;

private:
    struct Native;
    Pimpl<Native> impl;
};
} // namespace eacp::GPU
