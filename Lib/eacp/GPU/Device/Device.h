#pragma once

#include "../Buffer/Buffer.h"
#include "../CommandBuffer/CommandBuffer.h"
#include "../Pipeline/ComputePipeline.h"
#include "../Pipeline/RenderPipeline.h"
#include "../Shader/ShaderLibrary.h"
#include "../Shader/ShaderSource.h"
#include "../Texture/Sampler.h"
#include "../Texture/Texture.h"
#include "../Timing/FrameTimer.h"

#include <eacp/Core/Threads/ThreadUtils.h>

#include <cstdint>
#include <memory>
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
//
// The rule in full, because it is what assertOwningThread() below checks. A
// Device is owned by the thread that constructed it, and the process-wide
// Device::shared() is owned by the main thread whichever thread happened to ask
// for it first - every GPUView and every Frame drives that one from the main
// thread, so binding it to the first caller would be an accident of startup
// order. Everything a Device makes is used on its owning thread: creating a
// buffer, reading or updating one, beginning a frame, and submitting, waiting
// on or reading back a command buffer all assert it. A worker thread that wants
// the GPU makes a Device of its own and keeps the whole chain - buffers,
// pipelines, command buffers - on that thread; touching Device::shared() from
// there to compile a kernel is allowed and does not move its ownership. The
// check is one thread-id compare behind an assert, so a release build pays for
// nothing but the call.
class Device
{
public:
    Device();

    static Device& shared();

    // Fires a debug assertion when this Device is used from a thread that does
    // not own it. Called at the top of the operations that touch the backend;
    // an app may call it at the top of its own, on the same terms.
    void assertOwningThread() const;

    Buffer makeBuffer(const void* data,
                      std::int64_t bytes,
                      BufferUsage usage = BufferUsage::Vertex,
                      BufferStorage storage = BufferStorage::Device);

    template <typename T, std::size_t N>
    Buffer makeBuffer(const T (&array)[N], BufferUsage usage = BufferUsage::Vertex)
    {
        return makeBuffer(array, (std::int64_t) sizeof(array), usage);
    }

    // An uninitialised buffer of the given size, e.g. a compute output target.
    Buffer makeBuffer(std::int64_t bytes, BufferUsage usage = BufferUsage::Storage);

    // A buffer over memory the caller owns: shared with it where the backend
    // can, copied out of it where it cannot, which Buffer::canAdoptMemory
    // answers. The memory must be page-aligned in both address and length -
    // see ExternalMemory, which also carries the callback that frees it.
    //
    // What this is for is a file already in the address space. Mapping a
    // weights file and adopting the whole mapping makes every tensor in it a
    // BufferRange into one buffer, with nothing copied and nothing to keep in
    // step: the pages arrive as the GPU first touches them.
    Buffer makeBufferOverMemory(ExternalMemory memory,
                                BufferUsage usage = BufferUsage::Storage);

    // A 2D texture from tightly packed 4-byte pixels (row 0 at the top), or an
    // uninitialised texture when pixels is null.
    Texture makeTexture(const TextureDescriptor& descriptor,
                        const void* pixels = nullptr);

    // Any kind of texture, filled level by level and slice by slice: the
    // shape a container file (.dds, .ktx) hands over. See Texture's
    // constructor of the same shape. A name of its own rather than a
    // makeTexture overload, which a C array of subresources would decay past
    // into the pixel pointer.
    Texture makeTextureFromSubresources(
        const TextureDescriptor& descriptor,
        std::span<const TextureSubresource> subresources);

    // A 2D texture sized from a decoded image and uploaded from its RGBA8
    // pixels. The image is taken as tightly packed 8-bit RGBA (what
    // Graphics::Image holds), so the format is always RGBA8Unorm. An invalid or
    // empty image yields an invalid texture. Defined in Device.cpp.
    Texture makeTexture(const Graphics::Image& image);

    // Wraps an existing platform pixel buffer (a CVPixelBuffer on macOS) as a
    // sampleable texture without copying its pixels — the zero-copy path for
    // camera and video frames. Returns an invalid texture on backends without
    // zero-copy support (Windows for now), where Texture::update is the path.
    Texture wrapPixelBuffer(void* nativePixelBuffer);

    // The sampler a descriptor describes, made once per distinct descriptor and
    // kept for this Device's lifetime: asking again with an equal descriptor
    // returns the same native object, so a renderer can call this per draw. Safe
    // from any thread. Invalid on a device that is not, or when the backend
    // refused the descriptor.
    //
    // The four TextureSampling configurations are among these:
    // makeSampler(SamplerDescriptor::from(sampling)) is the sampler the
    // TextureSampling overloads bind.
    Sampler makeSampler(const SamplerDescriptor& descriptor);

    // Whether a Sampler bound per draw reaches the shader. True on Metal and
    // Vulkan; on D3D12 false where the driver ignores the offset of a sampler
    // descriptor table (see SAMPLERS.md), in which case the per-bind sampler
    // registers hold one fixed static sampler and a Sampler bind is dropped.
    bool supportsPerBindSamplers() const;

    ShaderLibrary makeShaderLibrary(const ShaderSource& source);

    RenderPipeline makeRenderPipeline(const RenderPipelineDescriptor& descriptor);

    ComputePipeline makeComputePipeline(const ShaderLibrary& library);

    CommandBuffer makeCommandBuffer();

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

    // The grid a ranged storage-buffer bind's offset has to sit on, in bytes -
    // ComputePass::setInputBuffer/setOutputBuffer over a BufferRange, and
    // RenderPass::setVertexStorageBuffer/setFragmentStorageBuffer over one. An
    // offset off it binds nothing, exactly as one past the buffer's end does.
    //
    // Four on Metal and D3D12, which take any word-aligned offset. Vulkan
    // writes the offset into a descriptor and the descriptor's alignment is a
    // device limit - 16 on Mesa's lavapipe, up to 256 by the spec - so a range
    // that a Mac takes is not automatically one this device takes, and code
    // that sub-allocates a buffer by row has to round the row to this rather
    // than to the element size. An invalid Device answers four.
    //
    // Only the storage binds: fill, dispatchIndirect and the vertex and index
    // ranges are four-byte everywhere, this backend included.
    int storageBufferOffsetAlignment() const;

    // How many storage-buffer slots a compute kernel may bind on this device:
    // ComputePass::maxBufferSlots (16) on Metal, the same or the device's own
    // per-stage storage-buffer limit on Vulkan, and
    // ComputePass::portableBufferSlots (8) on D3D12, whose root signature has no
    // room for more. A ComputePass bind at a slot at or past it is dropped. The
    // codegen layer stays inside the portable eight everywhere; this is for a
    // hand-written kernel that needs more. An invalid Device answers zero.
    int maxComputeBufferSlots() const;

    // Whether RenderPass::drawIndirectCount and drawIndexedIndirectCount, whose
    // number of draws is read from a buffer, reach the GPU. False on Metal,
    // which has no such draw; true on D3D12; on Vulkan where the device has
    // drawIndirectCount (core 1.2) or VK_KHR_draw_indirect_count. Where it is
    // false those calls are refused with a trace. The other indirect draws work
    // everywhere.
    bool supportsIndirectCount() const;

    // How many texture slots a render stage may bind on this device, each with
    // its per-bind sampler: RenderPass::maxStageTextureSlots (16) on Metal,
    // whose sixteen sampler indices are the ceiling though it has 31 texture
    // ones; the same on Vulkan, clamped to the device's per-stage sampler and
    // sampled-image limits (which the spec puts at 16 or more); and eight on
    // D3D12, whose texture registers stop under the storage buffers at t8 and
    // whose root signature has no room for more tables. A bind at a slot at or
    // past it is dropped. The codegen layer and compute keep to eight
    // everywhere. An invalid Device answers zero.
    int maxTextureSlots() const;

    // Whether RenderPass::setVertexTexture and setVertexDepthTexture reach a
    // vertex shader. True on every backend with a device.
    bool supportsVertexTextures() const;

    // Whether the vertex and fragment stages share one set of texture slots, so
    // that a vertex bind at slot n and a fragment bind at slot n are the same
    // binding and the last one wins. False on Metal, which has an index space
    // per stage; true on D3D12, whose texture and sampler tables are visible to
    // every stage, and on Vulkan, whose combined image samplers are declared
    // for both. Portable code binds one texture per slot across the two stages.
    bool stagesShareTextureSlots() const;

    // The most layers a TwoDArray, or cubes times six a CubeArray, may have:
    // 2048 on all three backends (Metal's, D3D12's required limit, Vulkan's
    // maxImageArrayLayers). 0 on an invalid Device.
    int maxTextureLayers() const;

    // The largest width, height or depth of a ThreeD texture: 2048 on Metal
    // and D3D12, maxImageDimension3D on Vulkan. 0 on an invalid Device.
    int maxTextureSize3D() const;

    // Whether TextureType::CubeArray textures can be created: every Mac and
    // Apple4 and later on Metal, every D3D12 device, Vulkan's imageCubeArray.
    bool supportsCubeArrays() const;

    // Whether a vertex shader may choose the layer a primitive lands in, which
    // is what a pass with ColorTarget::layerCount above 1 needs: every Mac and
    // Apple5 and later on Metal ([[render_target_array_index]]), D3D12's
    // VPAndRTArrayIndexFromAnyShaderFeedingRasterizerSupportedWithoutGSEmulation
    // (SV_RenderTargetArrayIndex), and Vulkan's shaderOutputLayer or
    // VK_EXT_shader_viewport_index_layer (gl_Layer). Without it a shader
    // writing the index is not a pipeline the device builds; a layered pass
    // still runs, its primitives landing in the first layer.
    bool supportsLayeredRendering() const;

    // How many bytes of threadgroup memory one group may declare on this
    // device - the budget every `shared<>` array in a kernel is spent out of,
    // the emitter's own reduction and SIMD-matrix scratch included.
    //
    // It is a device property on two of the three backends and a shader-model
    // constant on the third: Metal's maxThreadgroupMemoryLength is 32 KB on
    // every Mac eacp runs on and larger on some Apple-family parts, D3D12 at
    // cs_5_0 gives a group a flat 32 KB of groupshared, and Vulkan reports
    // maxComputeSharedMemorySize, which the spec floors at 16 KB. So 16 KB is
    // what a kernel may assume anywhere and 32 KB is what the two backends with
    // a fixed number give.
    //
    // Worth asking rather than knowing, because the alternative is what a
    // kernel author does today: carry the number in a comment, size the tile by
    // hand against it, and find out from a pipeline that would not build.
    // ComputeProgram::threadgroupMemoryBytes() is the other half - what the
    // kernel spends - and prepare() names the overspend before the backend
    // reports it as a pipeline it could not make. An invalid Device answers
    // zero, and a check against zero stands down.
    int maxThreadgroupMemory() const;

    // Whether this device loads an 8x8 SIMD-group matrix fragment out of a
    // buffer of packed sixteen-bit elements **natively** - one instruction, no
    // widening - which is what ComputeProgram::simdMatrixHalf and
    // simdMatrixBFloat16 emit on Metal.
    //
    // "Natively" is the whole of what these answer, and not "at all". They are
    // false on D3D12 and Vulkan, where the same two calls still build and still
    // compute the right thing: a fragment there is spread over the lanes, and a
    // packed load is each lane widening the pair it holds through the helper
    // every other packed read uses. Whether a program *builds* is
    // ComputeProgram::fitsPackedSimdMatrix, which is the check prepare() makes
    // and which only Metal can fail.
    //
    // So this is the question a kernel author asks **before building**, to
    // choose between two kernels: the packed load where the answer is yes, and
    // a staged threadgroup tile of widened floats where it is no. It is not a
    // branch to put inside a kernel. The staging path carries barriers the
    // packed path does not, and a barrier some threads in a group reach and
    // others do not is undefined - so the two cannot be the arms of one `if`.
    // On Windows and Linux both shapes build, and this answering no says the
    // staged one is the one worth having.
    //
    // fp16 fragments are Metal 2.3, so they are on the macOS 11 floor eacp
    // builds against; bf16 fragments are Metal 3.1 and need macOS 14 or iOS 17,
    // which is the whole reason these are two calls and not one. Both
    // additionally want the Apple-family GPU whose SIMD group is the 32 threads
    // the EDSL's tiling arithmetic is written against - ComputeProgram::
    // simdWidth. An invalid Device answers false to both.
    bool supportsHalfSimdMatrix() const;
    bool supportsBFloat16SimdMatrix() const;

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
    constexpr std::uint64_t frameIndex() const { return frameCount; }

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
    const FrameTimings& lastFrameTimings() const;

    // Whether this device can time individual passes. False says only that the
    // per-pass breakdown will be empty: FrameTimings::milliseconds, the frame
    // as a whole, is measured by other means and still arrives.
    //
    // Answerable only once a frame has begun, since that is what builds the
    // timestamp resources - ask after rendering, not before.
    bool supportsPassTimings() const;

    // Internal: the timer Frame drives. Apps read lastFrameTimings().
    constexpr FrameTimer& frameTimer() { return timer; }

    // How many GPU buffers have been created on this device since it came up.
    //
    // Per-frame data goes through StreamingBuffers, which recycles, so this
    // settles once a renderer's pools are warm. A count that keeps climbing
    // while the drawing repeats is allocation churn in the frame loop -
    // newBufferWithBytes on Metal, a committed resource on D3D12 - which is
    // what the assertions in Tests/GPU are there to catch.
    constexpr int buffersCreated() const { return bufferCount; }

    // Called by Buffer's constructor on both backends, for buffers that got
    // real storage.
    constexpr void noteBufferCreated() { ++bufferCount; }

private:
    // Makes this Device follow the main thread rather than the one that
    // constructed it. Private because Device::shared() is the only caller and
    // it is a member, so nothing outside can move a Device's ownership.
    constexpr void followMainThread() { mainThreadOwned = true; }

    struct Native;
    Pimpl<Native> impl;

    // Builds the backend's sampler for a descriptor and keeps it in Native.
    // Called by makeSampler for a descriptor not yet cached.
    void* createNativeSampler(const SamplerDescriptor& descriptor);

    struct SamplerCache;
    static std::shared_ptr<SamplerCache> makeSamplerCache();
    std::shared_ptr<SamplerCache> samplers = makeSamplerCache();

    FrameTimer timer;

    // The thread this Device was constructed on, and therefore the one it may
    // be used from - unless followMainThread() said to track the main thread
    // instead, which Device::shared() does.
    std::uint64_t owningThread = Threads::currentThreadId();
    bool mainThreadOwned = false;

    std::uint64_t frameCount = 0;
    int bufferCount = 0;
};
} // namespace eacp::GPU
