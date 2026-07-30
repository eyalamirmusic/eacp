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
    R8Unorm,

    // Two 8-bit channels, sampled as (r, g, 0, 1). Carries the interleaved
    // Cb/Cr plane of an NV12 video frame, whose luma plane is an R8Unorm of
    // twice the width and height.
    RG8Unorm,

    // Floating point, for a texture a shader writes as well as reads. Eight bits
    // per channel are enough to show a colour and not nearly enough to keep one:
    // a pass that feeds back into itself quantises every frame, so a value it
    // accumulates over hundreds of them - a trail, a simulation state, a running
    // average - drifts to a flat colour it can no longer leave.
    //
    // RGBA16Float is the one to reach for. Both backends can filter it on every
    // device eacp runs on, it holds well over the range a colour needs, and it
    // costs half of what the full float does. RGBA32Float is there for the
    // simulation that really needs the mantissa; filtering one is *not*
    // guaranteed, so sample it Nearest unless the device is known to allow more.
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

// Whether a kernel may write this format. The restriction is D3D12's: a typed
// UAV store is only guaranteed for a small set of formats, and everything
// outside it depends on the device. BGRA8Unorm - the drawable's own format - is
// not in the guaranteed set, which is exactly the trap this exists to close.
//
// A texture asked for computeWrite in another format fails to create rather
// than binding as a kernel output that quietly does nothing.
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

    // Whether a Frame can render into this texture as well as sample it -
    // Frame::beginPass(texture). Off by default because it is not free: the
    // resource has to be created able to be an attachment, which on D3D12 also
    // costs a render-target descriptor, and a texture that is only ever
    // uploaded to and sampled should not pay for either.
    //
    // A render target's pixels come from the GPU, so update() is not the way to
    // fill one and a null `pixels` is the only thing to create one with.
    bool renderTarget = false;

    // Whether a compute kernel can write into this texture - the other way its
    // pixels come from the GPU, and the one that lets a kernel produce
    // something a later pass samples. Off by default for the same reason
    // renderTarget is: the resource has to be created able to be one, which on
    // D3D12 also costs a UAV descriptor.
    //
    // Only the formats supportsComputeWrite() allows may ask for this, and only
    // on a device whose driver reports a typed UAV store for the format; a
    // texture that asks for it anywhere else is invalid rather than silently
    // unwritable. Unlike a render target this leaves update() alone, so a
    // kernel that accumulates can still be seeded from the CPU.
    bool computeWrite = false;

    // Whether to build the full chain of half-size levels down to 1x1, so the
    // GPU can sample a smaller one when the texture is drawn smaller than it is.
    //
    // What this buys is not speed but the picture. A texture minified without
    // mips samples a scattering of individual texels, and which texels those are
    // changes as the camera moves - so a tiled floor or a detailed model
    // shimmers and crawls at distance, and no amount of filtering at level 0
    // fixes it, because the information being aliased was thrown away before the
    // filter saw it.
    //
    // Off by default: a texture drawn at or above its own size - a UI atlas, a
    // video frame, a full-screen effect - never samples a level below 0, and
    // would pay a third more memory and upload for levels nothing reads.
    //
    // The levels are built on the CPU from the pixels passed in, so a texture
    // created with none of them (a render target, a kernel output) gets no
    // chain. update() rebuilds it; update(region, ...) does not - a partial
    // upload has no way to know what the rest of the texture holds, so it
    // refreshes level 0 and leaves the levels below it as they were.
    bool mipmapped = false;

    // Whether a pass rendering into this texture gets a depth buffer, which is
    // what a pipeline built with RenderPipelineDescriptor::depth tests against
    // and what a drawable pass already gets from GPUView::setDepth. Off by
    // default: a full-screen pass over a whole texture has no use for one, and
    // the buffer costs the colour texture's size again.
    //
    // Without it a 3D scene cannot be rendered into a texture at all. The depth
    // test has nothing to test against, so what comes out is painter's order -
    // which is the reason this exists, and why it is not the same question as
    // multisampling.
    //
    // The buffer is created beside the colour texture and lives exactly as
    // long, so a render target stays one object with no second lifetime to keep
    // in step. It is cleared to the far plane at the start of every pass and
    // never stored, like the drawable's. Ignored without renderTarget, which is
    // what renders.
    bool depth = false;
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

    // Whether this texture was created able to be rendered into, which is what
    // Frame::beginPass(texture) needs and what makes an ordinary one a no-op
    // there rather than undefined behaviour.
    bool isRenderTarget() const;

    // Whether a kernel can write into this texture - what
    // ComputePass::setOutputTexture needs, and false on a texture whose format
    // or device refused the request.
    bool isComputeWritable() const;

    // How many mip levels this texture actually has: 1 unless it asked for a
    // chain and got one. A format the chain builder does not know how to average
    // yields 1 rather than a texture whose lower levels are uninitialised.
    int mipLevels() const;

    // Whether a pass into this texture carries a depth buffer, which is what a
    // depth-tested pipeline needs and false on a target that did not ask for
    // one. A pass runs either way; without this it runs without the test.
    bool hasDepth() const;

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

    // The depth buffer a pass into this texture attaches, or null on a target
    // that asked for none. An MTLTexture on Metal; on D3D12 the depth resource
    // and its descriptor live inside the same texture data nativeTexture hands
    // back, so this is null there and Frame reaches them through that.
    void* nativeDepthTexture() const;

private:
    struct Native;
    Pimpl<Native> impl;
};
} // namespace eacp::GPU
