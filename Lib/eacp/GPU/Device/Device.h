#pragma once

#include "../Buffer/Buffer.h"
#include "../CommandBuffer/CommandBuffer.h"
#include "../Pipeline/ComputePipeline.h"
#include "../Pipeline/RenderPipeline.h"
#include "../Shader/ShaderLibrary.h"
#include "../Shader/ShaderSource.h"
#include "../Texture/Texture.h"

namespace eacp::Graphics
{
class Image;
}

namespace eacp::GPU
{
// The GPU device (MTLDevice + command queue on Metal). Owns the resource
// factories. Most apps use the process-wide Device::shared().
class Device
{
public:
    Device();

    static Device& shared();

    Buffer makeBuffer(const void* data,
                      std::size_t bytes,
                      BufferUsage usage = BufferUsage::Vertex)
    {
        return {*this, data, bytes, usage};
    }

    template <typename T, std::size_t N>
    Buffer makeBuffer(const T (&array)[N], BufferUsage usage = BufferUsage::Vertex)
    {
        return makeBuffer(array, sizeof(array), usage);
    }

    // An uninitialised buffer of the given size, e.g. a compute output target.
    Buffer makeBuffer(std::size_t bytes, BufferUsage usage = BufferUsage::Storage)
    {
        return {*this, nullptr, bytes, usage};
    }

    // A 2D texture from tightly packed 4-byte pixels (row 0 at the top), or an
    // uninitialised texture when pixels is null.
    Texture makeTexture(const TextureDescriptor& descriptor,
                        const void* pixels = nullptr)
    {
        return {*this, descriptor, pixels};
    }

    // A 2D texture sized from a decoded image and uploaded from its RGBA8
    // pixels. The image is taken as tightly packed 8-bit RGBA (what
    // Graphics::Image holds), so the format is always RGBA8Unorm. An invalid or
    // empty image yields an invalid texture. Defined in Device.cpp.
    Texture makeTexture(const Graphics::Image& image);

    // Wraps an existing platform pixel buffer (a CVPixelBuffer on macOS) as a
    // sampleable texture without copying its pixels — the zero-copy path for
    // camera and video frames. Returns an invalid texture on backends without
    // zero-copy support (Windows for now), where Texture::update is the path.
    Texture wrapPixelBuffer(void* nativePixelBuffer)
    {
        return {*this, nativePixelBuffer};
    }

    ShaderLibrary makeShaderLibrary(const ShaderSource& source)
    {
        return {*this, source};
    }

    RenderPipeline makeRenderPipeline(const RenderPipelineDescriptor& descriptor)
    {
        return {*this, descriptor};
    }

    ComputePipeline makeComputePipeline(const ShaderLibrary& library)
    {
        return {*this, library};
    }

    CommandBuffer makeCommandBuffer() { return CommandBuffer {*this}; }

    bool isValid() const;

    // Opaque native handles for cross-translation-unit use by other GPU types.
    void* nativeDevice() const;
    void* nativeQueue() const;

    // The Metal CVMetalTextureCache backing zero-copy pixel-buffer textures.
    // Null on backends without it (Windows), where wrapPixelBuffer is a no-op.
    void* nativeTextureCache() const;

    // The MTLSamplerState for one sampling configuration, built once and cached
    // for the device's lifetime — there are only samplingConfigurations of them,
    // and a render pass looks one up per texture bind. Null on D3D12, where the
    // sampler is static in the root signature and never bound at all.
    void* nativeSampler(TextureSampling sampling) const;

private:
    struct Native;
    Pimpl<Native> impl;
};
} // namespace eacp::GPU
