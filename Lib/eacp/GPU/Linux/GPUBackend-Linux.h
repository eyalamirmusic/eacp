#pragma once

#include "../Buffer/Buffer.h"
#include "../CommandBuffer/CommandBuffer.h"
#include "../Frame/ComputePass.h"
#include "../Frame/Frame.h"
#include "../Frame/RenderPass.h"
#include "../Pipeline/ComputePipeline.h"
#include "../Pipeline/RenderPipeline.h"
#include "../Shader/ShaderLibrary.h"
#include "../Texture/Texture.h"
#include "../Timing/GpuTimestamps.h"
#include "../View/GPUView.h"

#include <eacp/Graphics/View/View-Linux.h>

#include <memory>
#include <string>

// The seam the Linux backends meet the GPU classes at. One abstract struct per
// class, whose virtuals are that class's public methods one to one, so every
// X-Linux.cpp above this is a forwarder and every API call of a backend lives
// in that backend's own files.
//
// Which backend a Device got is asked exactly once, when it is made: the
// DeviceBackend makes every object under it, so nothing below a Device ever
// looks. The void* nativeX() handles keep returning the backend's own type,
// and every cast of one stays inside the files that know what it is.
//
// Linux only. Metal and D3D12 have one backend each and their Native structs
// stay concrete.
namespace eacp::GPU
{
// What a backend found when it went looking for a device, which is the whole of
// what the auto rule weighs (plan.md D8): nothing to open at all, a CPU
// implementation of the API, or hardware.
enum class GPUDeviceClass
{
    None,
    Software,
    Hardware
};

// Which of the two APIs a question is about. One backend speaks one of them
// and answers for the other with null; the composite (D11) speaks both and
// hands out the side that owns each.
enum class GPUApi
{
    Vulkan,
    OpenGL
};

// What a resource the composite gave two lives says to the composite's own
// passes, and the whole of what anything outside CompositeBackend-Linux.cpp
// knows about one (D11). Every single-API backend answers null to crossing()
// below, so a pass on such a backend never asks.
struct CrossingResource
{
    virtual ~CrossingResource() = default;

    // Said before the side records the write, not after: the flag says which
    // copy is behind, and the other side's next use is what pays for it.
    virtual void willWriteOnComputeSide() = 0;
    virtual void willWriteOnRenderSide() = 0;
};

struct BufferBackend
{
    virtual ~BufferBackend() = default;

    virtual std::int64_t size() const = 0;
    virtual bool isValid() const = 0;

    virtual void read(void* dst, std::int64_t bytes, std::int64_t offset) const = 0;
    virtual void
        update(const void* data, std::int64_t bytes, std::int64_t offset) = 0;
    virtual void updateUnordered(const void* data,
                                 std::int64_t bytes,
                                 std::int64_t offset) = 0;

    virtual void* nativeBuffer() const = 0;
    virtual void* nativeReadView() const = 0;
    virtual void* nativeWriteView() const = 0;

    // Null on every backend but the composite's (D11).
    virtual CrossingResource* crossing() { return nullptr; }
};

struct TextureBackend
{
    virtual ~TextureBackend() = default;

    virtual void update(const void* pixels, int bytesPerRow) = 0;
    virtual void updateRegion(int x,
                              int y,
                              int regionWidth,
                              int regionHeight,
                              const void* pixels,
                              int bytesPerRow) = 0;

    virtual void readRegion(int x,
                            int y,
                            int regionWidth,
                            int regionHeight,
                            void* dst,
                            int bytesPerRow) const = 0;

    virtual int width() const = 0;
    virtual int height() const = 0;
    virtual bool isValid() const = 0;
    virtual int mipLevels() const = 0;
    virtual bool isRenderTarget() const = 0;
    virtual bool isCube() const = 0;
    virtual bool isComputeWritable() const = 0;
    virtual bool hasDepth() const = 0;
    virtual bool hasStencil() const = 0;
    virtual bool hasSampleableDepth() const = 0;
    virtual int sampleCount() const = 0;

    virtual void* nativeTexture() const = 0;
    virtual void* nativeReadView() const = 0;
    virtual void* nativeDepthTexture() const = 0;
    virtual void* nativeMultisampleTexture() const = 0;
    virtual void* nativeResolvedDepthTexture() const = 0;

    // Null on every backend but the composite's (D11).
    virtual CrossingResource* crossing() { return nullptr; }
};

struct ShaderLibraryBackend
{
    virtual ~ShaderLibraryBackend() = default;

    virtual bool isValid() const = 0;
    virtual void* nativeLibrary() const = 0;
};

struct RenderPipelineBackend
{
    virtual ~RenderPipelineBackend() = default;

    virtual bool isValid() const = 0;
    virtual PrimitiveTopology topology() const = 0;
    virtual CullMode cullMode() const = 0;
    virtual Winding frontFace() const = 0;
    virtual void* nativeState() const = 0;
    virtual void* nativeDepthState() const = 0;
};

struct ComputePipelineBackend
{
    virtual ~ComputePipelineBackend() = default;

    virtual bool isValid() const = 0;
    virtual int threadExecutionWidth() const = 0;
    virtual void* nativeState() const = 0;
};

// The one pass method that is not the public one: the three dispatch overloads
// differ only in the extents they clamp and the group they encode with, and the
// group is the portable half's to decide.
struct ComputePassBackend
{
    virtual ~ComputePassBackend() = default;

    // Whether a pipeline was bound, which is what the portable half drops a
    // dispatch under.
    virtual bool setPipeline(const ComputePipeline& pipeline) = 0;

    virtual void setInputBuffer(const BufferRange& range, int slot) = 0;
    virtual void setOutputBuffer(const BufferRange& range, int slot) = 0;
    virtual void setInputTexture(const Texture& texture,
                                 int slot,
                                 TextureSampling sampling) = 0;
    virtual void setOutputTexture(const Texture& texture, int slot) = 0;
    virtual void setBytes(const void* data, std::int64_t bytes, int slot) = 0;

    virtual void
        dispatch(int width, int height, int depth, ThreadGroupShape group) = 0;
    virtual void dispatchIndirect(const Buffer& arguments,
                                  std::int64_t offsetInBytes) = 0;

    virtual void barrier() = 0;
    virtual void end() = 0;
};

struct RenderPassBackend
{
    virtual ~RenderPassBackend() = default;

    virtual int targetWidth() const = 0;
    virtual int targetHeight() const = 0;

    virtual void setScissorRect(const Graphics::Rect& rect) = 0;
    virtual void clearScissorRect() = 0;
    virtual void
        setViewport(const Graphics::Rect& rect, float nearDepth, float farDepth) = 0;
    virtual void clearViewport() = 0;

    virtual void setPipeline(const RenderPipeline& pipeline) = 0;
    virtual void setStencilReference(unsigned int value) = 0;
    virtual void setVertexBuffer(const BufferRange& range, int index) = 0;
    virtual void setFragmentTexture(const Texture& texture,
                                    int slot,
                                    TextureSampling sampling) = 0;
    virtual void setFragmentDepthTexture(const Texture& renderTarget,
                                         int slot,
                                         TextureSampling sampling) = 0;
    virtual void setStorageBuffer(const BufferRange& range, int slot) = 0;
    virtual void setBytes(const void* data, int bytes, int slot) = 0;

    virtual void drawInstanced(int vertexCount,
                               int instanceCount,
                               int firstVertex,
                               int firstInstance) = 0;
    virtual void drawIndexedInstanced(const BufferRange& indices,
                                      int indexCount,
                                      int instanceCount,
                                      IndexFormat format,
                                      int firstIndex,
                                      int firstInstance,
                                      int baseVertex) = 0;

    virtual void end() = 0;
};

struct FrameBackend
{
    virtual ~FrameBackend() = default;

    virtual bool isValid() const = 0;
    virtual Graphics::Point pixelSize() const = 0;

    // The frame's own timestamp, written before anything else is recorded.
    virtual void beginTiming() = 0;

    virtual void flush() = 0;

    // Null where the frame has no target to draw into, which is what the
    // portable half turns into a RenderPass over no encoder.
    virtual std::unique_ptr<RenderPassBackend>
        beginPass(const RenderPassDescriptor& descriptor) = 0;
    virtual std::unique_ptr<RenderPassBackend>
        beginPass(const Texture& target, const RenderPassDescriptor& descriptor) = 0;

    virtual std::unique_ptr<ComputePassBackend>
        beginCompute(std::string_view label, DispatchOrder order) = 0;
};

struct CommandBufferBackend
{
    virtual ~CommandBufferBackend() = default;

    virtual bool isValid() const = 0;

    virtual std::unique_ptr<ComputePassBackend>
        beginCompute(std::string_view label, DispatchOrder order) = 0;

    virtual void fill(const BufferRange& range, std::uint8_t value) = 0;

    virtual void submit() = 0;
    virtual void wait() = 0;
    virtual bool isComplete() const = 0;
    virtual Threads::Async<void> commitAsync() = 0;

    virtual const FrameTimings& timings() = 0;
    virtual bool supportsPassTimings() const = 0;
};

struct GpuTimestampsBackend
{
    virtual ~GpuTimestampsBackend() = default;

    virtual bool isSupported() const = 0;
    virtual void beginSlot(int slot, Device& device) = 0;
    virtual void beginRecording(int slot, void* nativeCommandBuffer) = 0;
    virtual void* nativeSamples(int slot) const = 0;
    virtual bool endSlot(int slot, int passCount, void* nativeCommandBuffer) = 0;
    virtual void noteSubmitted(int slot, std::uint64_t fenceValue) = 0;
    virtual bool isSlotComplete(int slot, const Device& device) const = 0;
    virtual double resolveSlot(int slot, int passCount, double* milliseconds) = 0;
};

// The half of a GPUView that owns a drawable. The pacing above it - the frame
// callbacks, the max-fps divider, the continuous tick and the off-screen
// renderNativeContent - is window-system logic neither API owns and stays in
// GPUView-Linux.cpp.
struct GPUViewBackend
{
    virtual ~GPUViewBackend() = default;

    // The surface came up: build what presents to it. False leaves the view
    // with nothing to draw into, which is also the headless state.
    virtual bool surfaceAvailable() = 0;

    // The surface is about to go; a drawable that outlives it is a
    // use-after-free inside the driver.
    virtual void surfaceLost() = 0;

    // The surface's size changed under what was built for it.
    virtual void surfaceResized() = 0;

    // Whether there is something to present to, which is what a continuous
    // tick drops a frame on.
    virtual bool isPresenting() const = 0;

    // Whether a frame can be drawn now, rebuilding whatever went stale.
    virtual bool readyToRender() = 0;

    // Acquires, renders the view into the drawable and presents it.
    virtual void renderOneFrame(float scale) = 0;

    virtual void setSampleCount(int count) = 0;
    virtual void setDepth(bool depth, bool stencil) = 0;
    virtual void setFramesInFlight(int count) = 0;

    // Fired once when the device is lost, so the view stops pacing. The
    // backend has already torn down everything it owns by then.
    Callback onDeviceLost = [] {};
};

struct DeviceBackend
{
    virtual ~DeviceBackend() = default;

    // What this backend calls itself - "Vulkan", "OpenGL" - beside the name of
    // the device it opened.
    virtual std::string backendName() const = 0;

    // The backend that runs one API's work under this Device: itself where it
    // is that API, the matching half of a composite (D11), and null where this
    // Device has no such side - which is a question nothing asks, every
    // -Vulkan.cpp and -GL.cpp being reached only through a Device that has one.
    virtual DeviceBackend* sideFor(GPUApi api) = 0;

    virtual bool isValid() const = 0;
    virtual std::string name() const = 0;

    virtual bool supportsCompute() const = 0;
    virtual bool supportsStorageBuffers() const = 0;
    virtual bool supportsZeroToOneDepth() const = 0;
    virtual bool supportsSampleCount(int count) const = 0;
    virtual bool supportsBlockCompression() const = 0;
    virtual int storageBufferOffsetAlignment() const = 0;
    virtual int maxThreadgroupMemory() const = 0;
    virtual bool supportsHalfSimdMatrix() const = 0;
    virtual bool supportsBFloat16SimdMatrix() const = 0;

    virtual void* nativeContext() const = 0;
    virtual void* nativeDevice() const = 0;
    virtual void* nativeQueue() const = 0;
    virtual void* nativeTextureCache() const = 0;
    virtual void* nativeSampler(TextureSampling sampling) const = 0;

    virtual void followMainThread() = 0;
    virtual void waitForSubmittedWork() = 0;

    virtual std::unique_ptr<BufferBackend> makeBuffer(Device& device,
                                                      const void* data,
                                                      std::int64_t bytes,
                                                      BufferUsage usage,
                                                      BufferStorage storage) = 0;

    virtual std::unique_ptr<TextureBackend> makeTexture(
        Device& device, const TextureDescriptor& descriptor, const void* pixels) = 0;

    virtual std::unique_ptr<TextureBackend>
        wrapPixelBuffer(Device& device, void* nativePixelBuffer) = 0;

    virtual std::unique_ptr<ShaderLibraryBackend>
        makeShaderLibrary(Device& device, const ShaderSource& source) = 0;

    virtual std::unique_ptr<RenderPipelineBackend>
        makeRenderPipeline(Device& device,
                           const RenderPipelineDescriptor& descriptor) = 0;

    virtual std::unique_ptr<ComputePipelineBackend>
        makeComputePipeline(Device& device, const ShaderLibrary& library) = 0;

    virtual std::unique_ptr<CommandBufferBackend>
        makeCommandBuffer(Device& device) = 0;

    virtual std::unique_ptr<FrameBackend> makeFrame(Device& device,
                                                    void* drawable) = 0;

    virtual std::unique_ptr<FrameBackend>
        makeFrame(Device& device, const OffscreenTarget& target) = 0;

    virtual std::unique_ptr<GpuTimestampsBackend> makeGpuTimestamps() = 0;

    virtual std::unique_ptr<GPUViewBackend>
        makeGPUView(GPUView& view, Graphics::ViewSurface& record) = 0;
};

// The Device's own backend, for the objects it makes. Every Linux Native
// struct that is handed a Device goes through this and nothing else.
DeviceBackend& getDeviceBackend(const Device& device);

// The half of it that speaks one API, which is what getVulkanContext and
// getGLContext read: a composite Device has one of each and a single-API one
// is its own.
DeviceBackend& getDeviceBackend(const Device& device, GPUApi api);

// The backend behind a public object, for the one backend that wraps another.
// Defined in each class's -Linux.cpp beside its Native, and read by nothing
// but CompositeBackend-Linux.cpp.
BufferBackend& getBufferBackend(const Buffer& buffer);
TextureBackend& getTextureBackend(const Texture& texture);
} // namespace eacp::GPU
