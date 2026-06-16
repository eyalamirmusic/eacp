#pragma once

#include <eacp/Core/Utils/Common.h>

#include <cstddef>

namespace eacp::GPU
{
class Device;

enum class TextureFormat
{
    RGBA8Unorm,
    BGRA8Unorm
};

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

// How the texture is created and sampled. The sampler state (filter, address
// mode) is baked into the texture for now, so binding a texture binds its
// sampler with it; standalone sampler objects can come later if mixing
// filters per draw becomes necessary.
struct TextureDescriptor
{
    int width = 0;
    int height = 0;
    TextureFormat format = TextureFormat::RGBA8Unorm;
    TextureFilter filter = TextureFilter::Linear;
    TextureAddressMode addressMode = TextureAddressMode::Clamp;
};

// A 2D texture sampled by the fragment stage (MTLTexture on Metal, a D3D12
// resource with its SRV and sampler descriptors on Windows). Create via
// Device::makeTexture with tightly packed 4-byte pixels, row 0 at the top, or
// null pixels for an uninitialised texture. Bind with
// RenderPass::setFragmentTexture.
class Texture
{
public:
    Texture(Device& device, const TextureDescriptor& descriptor, const void* pixels);

    // Wraps an existing platform pixel buffer (a CVPixelBuffer on macOS) as a
    // sampleable texture without copying its pixels — the zero-copy path for
    // camera and video frames. The buffer must outlive the texture. Yields an
    // invalid texture on backends without zero-copy support (Windows for now),
    // where update() is the per-frame upload path instead.
    Texture(Device& device,
            void* nativePixelBuffer,
            TextureFilter filter,
            TextureAddressMode addressMode);

    int width() const;
    int height() const;
    bool isValid() const;

    // Re-uploads pixels into a texture created by Device::makeTexture, reusing
    // the GPU resource instead of allocating a new one — the per-frame path for
    // video and camera streams. Source rows are tightly packed 4-byte pixels
    // unless bytesPerRow gives a larger stride (0 means width * 4), matching the
    // padded rows capture buffers often carry. A no-op on a wrapped or invalid
    // texture, or when pixels is null.
    void update(const void* pixels, std::size_t bytesPerRow = 0);

    // Opaque native handles for cross-translation-unit use by the render pass.
    void* nativeTexture() const;
    void* nativeSampler() const;

    // The read view the fragment stage binds on D3D12 (the same handle as
    // nativeTexture). Null on Metal, where the texture is bound directly.
    void* nativeReadView() const;

private:
    struct Native;
    Pimpl<Native> impl;
};
} // namespace eacp::GPU
