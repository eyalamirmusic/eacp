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
#include <string>

namespace eacp::Graphics
{
class Image;
}

namespace eacp::GPU
{
// The GPU device (MTLDevice + command queue on Metal). Owns the resource
// factories. Most apps use the process-wide Device::shared().
//
// A Device is single-threaded, and a thread that wants the GPU without queueing
// behind the main one makes its own:
//
//     auto worker = GPU::Device();
//
// Everything that Device creates belongs to it — a Buffer, a Texture and a
// CommandBuffer made from one are not usable from another, on either backend
// (an MTLBuffer belongs to its MTLDevice; a D3D12 recording to its queue's
// pool). Using a Device off the thread that constructed it is a debug assertion
// rather than a race left to be found later.
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

    // What the GPU this Device runs on calls itself — the MTLDevice's name on
    // Metal, the DXGI adapter description on D3D12. For a log line or a
    // benchmark header, which has to say which hardware produced a number, and
    // is worth having as a call rather than as a platform ifdef in every app
    // that prints one. An invalid Device names itself rather than returning
    // nothing, so a caller can print it either way.
    std::string name() const;

    // Whether a render target of this many samples can be created on this
    // device - TextureDescriptor::sampleCount, and the drawable's
    // GPUView::setSampleCount.
    //
    bool supportsSampleCount(int count) const;

    // Whether the block-compressed formats - BC1, BC2, BC3 and BC7 - can be
    // created on this device. Every Mac answers yes, and every Direct3D device
    // eacp runs on is required to; an Apple-family iOS GPU mostly answers no.
    //
    // Below macOS 11 or iOS 16.4 this answers no because the *query* does not
    // exist there, not because the hardware lacks the formats - Metal has had
    // them on macOS since 10.11. Answering no is eacp declining to guess: a
    // caller that gets a yes knows the texture will be created, and a caller
    // that gets a no on such a system keeps whatever it would have kept anyway.
    //
    // Here for the reason supportsSampleCount is: a texture in a format the
    // device refuses is **invalid** rather than quietly something else, so a
    // caller with a choice to make - keep the uncompressed original, or decline
    // the file - has to make it before it asks for the texture rather than by
    // finding out afterwards.
    bool supportsBlockCompression() const;

    // Opaque native handles for cross-translation-unit use by other GPU types.
    void* nativeDevice() const;
    void* nativeQueue() const;

    // The backend's per-Device state: a D3D12Context on Windows, which every
    // Windows translation unit reaches through getD3D12Context(device). Null on
    // Metal, where the queue and the caches are members of Device::Native and
    // the native handles above are all anything needs.
    void* nativeContext() const;

    // The Metal CVMetalTextureCache backing zero-copy pixel-buffer textures.
    // Null on backends without it (Windows), where wrapPixelBuffer is a no-op.
    void* nativeTextureCache() const;

    // The MTLSamplerState for one sampling configuration, built once and cached
    // for the device's lifetime — there are only samplingConfigurations of them,
    // and a render pass looks one up per texture bind. Null on D3D12, where the
    // sampler is static in the root signature and never bound at all.
    void* nativeSampler(TextureSampling sampling) const;

    // Remembers the newest submission, and blocks until it has finished.
    //
    // The queue is FIFO, so waiting for the newest submission waits for every
    // earlier one too. That is what keeps Buffer::read correct now that
    // CommandBuffer::commitAsync returns without waiting: the read blocks for
    // exactly as long as the work it depends on still needs, and not at all
    // once that work is done.
    //
    // Called by whatever commits — CommandBuffer and Frame. On D3D12 the queue's
    // fence already records this, so tracking is a no-op there and the wait goes
    // to the fence.
    void trackSubmittedWork(void* nativeCommandBuffer);
    void waitForSubmittedWork();

    // How many frames have begun on this device. StreamingBuffers picks which
    // of its pools to write into from this, so that a renderer streaming
    // per-frame data has nothing to call at the frame boundary and therefore
    // nothing to forget - see StreamingBuffers for why that matters.
    std::uint64_t frameIndex() const { return frameCount; }

    // Called by Frame's constructor on both backends, including the off-screen
    // one. An off-screen frame blocks until the GPU is done, so nothing it
    // wrote is in flight afterwards and the advance is not needed for
    // correctness - but without it a loop of off-screen renders is one endless
    // frame to StreamingBuffers, which then takes a fresh buffer every pass and
    // never reclaims one.
    //
    // Out of line because it also starts the frame timer, which is the one
    // thing both backends want done identically at this moment.
    void beginFrame();

    // What the GPU spent on the most recent frame it has finished: every pass
    // that was given a label, plus the frame end to end.
    //
    // A pass is timed by giving it one:
    //
    //     auto pass = frame.beginPass({.label = "ui"});
    //
    // An unlabelled pass is not timed and costs nothing. The numbers are a few
    // frames behind whatever is being drawn now, and cannot be anything else -
    // see FrameTimings for why.
    const FrameTimings& lastFrameTimings() const { return timer.lastTimings(); }

    // Whether this device can time individual passes. False says only that the
    // per-pass breakdown will be empty: FrameTimings::milliseconds, the frame
    // as a whole, is measured by other means and still arrives.
    //
    // Answerable only once a frame has begun, since that is what builds the
    // timestamp resources - ask after rendering, not before.
    bool supportsPassTimings() const { return timer.isSupported(); }

    // Internal: the timer Frame drives. Apps read lastFrameTimings().
    FrameTimer& frameTimer() { return timer; }

    // How many GPU buffers have been created on this device since it came up.
    //
    // Per-frame data goes through StreamingBuffers, which recycles, so this
    // settles once a renderer's pools are warm. A count that keeps climbing
    // while the drawing repeats is allocation churn in the frame loop -
    // newBufferWithBytes on Metal, a committed resource on D3D12 - which is
    // what the assertions in Tests/GPU are there to catch.
    int buffersCreated() const { return bufferCount; }

    // Called by Buffer's constructor on both backends, for buffers that got
    // real storage.
    void noteBufferCreated() { ++bufferCount; }

private:
    struct Native;
    Pimpl<Native> impl;

    FrameTimer timer;

    std::uint64_t frameCount = 0;
    int bufferCount = 0;
};
} // namespace eacp::GPU
