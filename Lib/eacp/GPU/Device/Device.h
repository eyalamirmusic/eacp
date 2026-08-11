#pragma once

#include "../Buffer/Buffer.h"
#include "../CommandBuffer/CommandBuffer.h"
#include "../Pipeline/ComputePipeline.h"
#include "../Pipeline/RenderPipeline.h"
#include "../Shader/ShaderLibrary.h"
#include "../Shader/ShaderSource.h"
#include "../Texture/Texture.h"
#include "../Timing/FrameTimer.h"

#include <cstdint>

namespace eacp::Graphics
{
class Image;
}

namespace eacp::GPU
{
// The GPU device and its command queue; most apps use Device::shared().
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

    // pixels is tightly packed 4-byte rows, row 0 at the top; null leaves the
    // texture uninitialised.
    Texture makeTexture(const TextureDescriptor& descriptor,
                        const void* pixels = nullptr)
    {
        return {*this, descriptor, pixels};
    }

    // Always RGBA8Unorm, matching Graphics::Image's packed 8-bit RGBA.
    Texture makeTexture(const Graphics::Image& image);

    // Zero-copy wrap of a platform pixel buffer (CVPixelBuffer on macOS) for
    // camera/video frames. Invalid texture on Windows; use Texture::update.
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

    void* nativeDevice() const;
    void* nativeQueue() const;

    // The CVMetalTextureCache behind wrapPixelBuffer; null on Windows.
    void* nativeTextureCache() const;

    // Null on D3D12, where samplers are static in the root signature.
    void* nativeSampler(TextureSampling sampling) const;

    // The queue is FIFO, so waiting for the newest submission waits for every
    // earlier one. On D3D12 tracking is a no-op; the wait goes to the fence.
    void trackSubmittedWork(void* nativeCommandBuffer);
    void waitForSubmittedWork();

    // Frames begun on this device; StreamingBuffers picks its pool from this.
    std::uint64_t frameIndex() const { return frameCount; }

    // Called by Frame's constructor on both backends, off-screen included.
    void beginFrame();

    // The most recent finished frame: every labelled pass, plus the frame end
    // to end. Unlabelled passes are not timed. Runs a few frames behind.
    const FrameTimings& lastFrameTimings() const { return timer.lastTimings(); }

    // Only the per-pass breakdown depends on this; the whole-frame time always
    // arrives. Answerable only once a frame has begun.
    bool supportsPassTimings() const { return timer.isSupported(); }

    // Internal: the timer Frame drives. Apps read lastFrameTimings().
    FrameTimer& frameTimer() { return timer; }

    // Settles once StreamingBuffers pools are warm; a climbing count is
    // allocation churn in the frame loop.
    int buffersCreated() const { return bufferCount; }

    void noteBufferCreated() { ++bufferCount; }

private:
    struct Native;
    Pimpl<Native> impl;

    FrameTimer timer;

    std::uint64_t frameCount = 0;
    int bufferCount = 0;
};
} // namespace eacp::GPU
