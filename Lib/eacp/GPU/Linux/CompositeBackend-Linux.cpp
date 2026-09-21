#include "CompositeBackend-Linux.h"

#include "../Device/Device.h"
#include "../OpenGL/GLBackend-Linux.h"
#include "../Vulkan/VulkanBackend-Linux.h"

#include <eacp/Core/Utils/Logging.h>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <vector>

namespace eacp::GPU
{
namespace
{
// Which half a native handle is being asked for. Render is the resting state:
// a composite Device draws through OpenGL, and only an open compute pass - or
// the one command-buffer call that records a transfer beside one - raises it.
enum class Side
{
    Render,
    Compute
};

// What a copy between the halves is timed on. The clock pair is one per
// crossing, not one per byte, and a crossing already costs a read-back and an
// upload, so what it measures is free against what it measures.
using Clock = std::chrono::steady_clock;

// What one side still owes the other, for a buffer. Byte offsets, unioned, so
// a host write of one row and a kernel writing the lot are the same two
// numbers.
struct DirtyRange
{
    bool isEmpty() const { return end <= begin; }

    void clear() { *this = DirtyRange {}; }

    void add(std::int64_t from, std::int64_t to)
    {
        if (to <= from)
            return;

        if (isEmpty())
        {
            begin = from;
            end = to;
            return;
        }

        begin = std::min(begin, from);
        end = std::max(end, to);
    }

    std::int64_t begin = 0;
    std::int64_t end = 0;
};

// The same for a texture, as a rectangle: D11's dirty rectangle where the
// write reported one and the whole resource where it did not.
struct DirtyRegion
{
    bool isEmpty() const { return width <= 0 || height <= 0; }

    void clear() { *this = DirtyRegion {}; }

    void add(int x, int y, int addedWidth, int addedHeight)
    {
        if (addedWidth <= 0 || addedHeight <= 0)
            return;

        if (isEmpty())
        {
            left = x;
            top = y;
            width = addedWidth;
            height = addedHeight;
            return;
        }

        const auto right = std::max(left + width, x + addedWidth);
        const auto bottom = std::max(top + height, y + addedHeight);

        left = std::min(left, x);
        top = std::min(top, y);
        width = right - left;
        height = bottom - top;
    }

    int left = 0;
    int top = 0;
    int width = 0;
    int height = 0;
};

struct ComputeRecording;

// What the two halves and everything made from them share: the backends, which
// side is asking right now, what the compute half has recorded and not yet
// submitted, and the Device whose per-frame crossing counters this feeds.
struct CompositeShared
{
    void noteCrossing(std::int64_t bytes, Clock::time_point started)
    {
        if (owner == nullptr || bytes <= 0)
            return;

        const auto spent = Clock::now() - started;

        owner->noteCrossing(
            bytes, std::chrono::duration<double, std::milli>(spent).count());
    }

    // Submits and waits for every recording the compute half has open, which is
    // what a render-side use of a kernel's output goes through before it reads
    // one. Defined below ComputeRecording.
    void finishComputeWork();

    std::unique_ptr<DeviceBackend> render;
    std::unique_ptr<DeviceBackend> compute;

    Side side = Side::Render;

    // Every recording that has something in it: a Frame's, a CommandBuffer's,
    // or both where an app has one of each in flight.
    std::vector<ComputeRecording*> openRecordings;

    Device* owner = nullptr;
};

// Raises the side for as long as it is alive, which is the whole of how a
// crossing resource knows which of its two handles is being asked for.
struct SideGuard
{
    SideGuard(CompositeShared& sharedToUse, Side wanted)
        : shared(sharedToUse)
        , previous(sharedToUse.side)
    {
        shared.side = wanted;
    }

    ~SideGuard() { shared.side = previous; }

    SideGuard(const SideGuard&) = delete;
    SideGuard& operator=(const SideGuard&) = delete;

    CompositeShared& shared;
    Side previous;
};

// A buffer with a life on each side. The render one is made with the buffer -
// it is the one a draw binds, and the one a Device with no kernels would have
// had - and the compute one the first time a kernel asks, filled from the
// render side as it is made (D11).
struct CompositeBuffer final
    : BufferBackend
    , CrossingResource
{
    CompositeBuffer(CompositeShared& sharedToUse,
                    Device& device,
                    const void* data,
                    std::int64_t bytes,
                    BufferUsage usage,
                    BufferStorage storage)
        : shared(sharedToUse)
        , owner(&device)
        , render(sharedToUse.render->makeBuffer(device, data, bytes, usage, storage))
        , renderHasContents(data != nullptr)
    {
    }

    // Device storage and every usage bit whatever the render side asked for: a
    // twin is there to be written by a kernel, which is what a host mapping
    // cannot carry.
    BufferBackend* computeSide() const
    {
        if (compute == nullptr && size() > 0 && shared.compute != nullptr)
        {
            compute = shared.compute->makeBuffer(*owner,
                                                 nullptr,
                                                 size(),
                                                 BufferUsage::Storage,
                                                 BufferStorage::Device);

            // Only where the render side holds something worth having. A
            // buffer created empty and bound as a kernel's output has nothing
            // to carry across, and copying its undefined bytes would be a
            // crossing charged for nothing.
            if (renderHasContents)
                computeStale.add(0, size());
        }

        syncToCompute();

        return compute.get();
    }

    BufferBackend* renderSide() const
    {
        syncToRender();

        return render.get();
    }

    BufferBackend* sideNow() const
    {
        return shared.side == Side::Compute ? computeSide() : renderSide();
    }

    void syncToCompute() const
    {
        if (computeStale.isEmpty() || compute == nullptr || !compute->isValid()
            || render == nullptr || !render->isValid())
            return;

        const auto offset = computeStale.begin;
        const auto count = computeStale.end - offset;
        const auto started = Clock::now();

        computeStale.clear();

        staging.resize((std::size_t) count);
        render->read(staging.data(), count, offset);
        compute->updateUnordered(staging.data(), count, offset);

        shared.noteCrossing(count, started);
    }

    void syncToRender() const
    {
        if (renderStale.isEmpty() || compute == nullptr || !compute->isValid()
            || render == nullptr || !render->isValid())
            return;

        // Nothing can be read off the compute side while its recording is
        // still open, so this is where the dispatches are submitted and waited
        // for - D11's "beginPass waits for the Vulkan work", reached from the
        // first use rather than from the pass boundary.
        shared.finishComputeWork();

        const auto offset = renderStale.begin;
        const auto count = renderStale.end - offset;
        const auto started = Clock::now();

        renderStale.clear();

        staging.resize((std::size_t) count);
        compute->read(staging.data(), count, offset);
        render->updateUnordered(staging.data(), count, offset);

        shared.noteCrossing(count, started);
    }

    void willWriteOnComputeSide() override { renderStale.add(0, size()); }

    void willWriteOnRenderSide() override
    {
        renderHasContents = true;

        if (compute != nullptr)
            computeStale.add(0, size());
    }

    std::int64_t size() const override
    {
        return render != nullptr ? render->size() : 0;
    }

    bool isValid() const override { return render != nullptr && render->isValid(); }

    // Read off whichever side holds the newest bytes, so that checking what a
    // kernel wrote does not drag the whole resource across first.
    void read(void* dst, std::int64_t bytes, std::int64_t offset) const override
    {
        if (!renderStale.isEmpty() && compute != nullptr)
        {
            shared.finishComputeWork();
            compute->read(dst, bytes, offset);
            return;
        }

        if (render != nullptr)
            render->read(dst, bytes, offset);
    }

    void update(const void* data, std::int64_t bytes, std::int64_t offset) override
    {
        if (render == nullptr)
            return;

        render->update(data, bytes, offset);
        noteHostWrite(bytes, offset);
    }

    void updateUnordered(const void* data,
                         std::int64_t bytes,
                         std::int64_t offset) override
    {
        if (render == nullptr)
            return;

        render->updateUnordered(data, bytes, offset);
        noteHostWrite(bytes, offset);
    }

    // A host write lands on the render side and leaves the twin owing exactly
    // the bytes it covered, which is the dirty-range half of D11's rule.
    void noteHostWrite(std::int64_t bytes, std::int64_t offset)
    {
        if (bytes <= 0)
            return;

        renderHasContents = true;

        if (compute == nullptr)
            return;

        renderStale.clear();
        computeStale.add(offset, offset + bytes);
    }

    void* nativeBuffer() const override
    {
        auto* backend = sideNow();

        return backend != nullptr ? backend->nativeBuffer() : nullptr;
    }

    void* nativeReadView() const override
    {
        auto* backend = sideNow();

        return backend != nullptr ? backend->nativeReadView() : nullptr;
    }

    void* nativeWriteView() const override
    {
        auto* backend = sideNow();

        return backend != nullptr ? backend->nativeWriteView() : nullptr;
    }

    CrossingResource* crossing() override { return this; }

    CompositeShared& shared;
    Device* owner = nullptr;

    std::unique_ptr<BufferBackend> render;

    // Mutable because every native handle is asked for through a const
    // accessor, and answering one is what makes the twin and squares it.
    mutable std::unique_ptr<BufferBackend> compute;
    mutable DirtyRange renderStale;
    mutable DirtyRange computeStale;

    // Whether anything has put bytes in the render side worth carrying over
    // when the twin is made.
    mutable bool renderHasContents = false;
    mutable std::vector<std::byte> staging;
};

// A texture with a life on each side. computeWrite is stripped from the render
// one - an image store is exactly what the OpenGL this device was built for
// has not got - and carried by the compute one, which is made with the texture
// where the descriptor asked for it and lazily where a kernel merely reads it.
struct CompositeTexture final
    : TextureBackend
    , CrossingResource
{
    CompositeTexture(CompositeShared& sharedToUse,
                     Device& device,
                     const TextureDescriptor& descriptorToUse,
                     const void* pixels)
        : shared(sharedToUse)
        , owner(&device)
        , descriptor(descriptorToUse)
    {
        auto renderDescriptor = descriptor;
        renderDescriptor.computeWrite = false;

        render = shared.render->makeTexture(device, renderDescriptor, pixels);
        renderHasContents = pixels != nullptr;

        if (descriptor.computeWrite)
            makeComputeTwin();
    }

    // The other way round: the compute side is the one that may be written, so
    // it keeps computeWrite and gives up being an attachment and having levels,
    // neither of which a kernel reaches.
    void makeComputeTwin() const
    {
        if (compute != nullptr || shared.compute == nullptr)
            return;

        if (isCompressedFormat(descriptor.format) || descriptor.mipLevels > 0)
            return;

        auto twin = descriptor;
        twin.renderTarget = false;
        twin.mipmapped = false;
        twin.sampleCount = 1;

        compute = shared.compute->makeTexture(*owner, twin, nullptr);

        // As for a buffer: a twin owes the render side's texels only where
        // there are texels worth owing.
        if (renderHasContents)
            computeStale.add(0, 0, descriptor.width, descriptor.height);
    }

    TextureBackend* computeSide() const
    {
        makeComputeTwin();
        syncToCompute();

        return compute.get();
    }

    TextureBackend* renderSide() const
    {
        syncToRender();

        return render.get();
    }

    TextureBackend* sideNow() const
    {
        return shared.side == Side::Compute ? computeSide() : renderSide();
    }

    std::int64_t regionBytes(const DirtyRegion& region) const
    {
        const auto rowBytes = levelBytesPerRow(descriptor.format, region.width);

        return (std::int64_t) rowBytes * region.height;
    }

    void syncToCompute() const
    {
        if (computeStale.isEmpty() || compute == nullptr || !compute->isValid()
            || render == nullptr || !render->isValid())
            return;

        const auto region = computeStale;
        const auto started = Clock::now();

        computeStale.clear();

        const auto bytes = regionBytes(region);
        const auto rowBytes = levelBytesPerRow(descriptor.format, region.width);

        staging.resize((std::size_t) bytes);

        render->readRegion(region.left,
                           region.top,
                           region.width,
                           region.height,
                           staging.data(),
                           rowBytes);

        compute->updateRegion(region.left,
                              region.top,
                              region.width,
                              region.height,
                              staging.data(),
                              rowBytes);

        shared.noteCrossing(bytes, started);
    }

    void syncToRender() const
    {
        if (renderStale.isEmpty() || compute == nullptr || !compute->isValid()
            || render == nullptr || !render->isValid())
            return;

        shared.finishComputeWork();

        const auto region = renderStale;
        const auto started = Clock::now();

        renderStale.clear();

        const auto bytes = regionBytes(region);
        const auto rowBytes = levelBytesPerRow(descriptor.format, region.width);

        staging.resize((std::size_t) bytes);

        compute->readRegion(region.left,
                            region.top,
                            region.width,
                            region.height,
                            staging.data(),
                            rowBytes);

        render->updateRegion(region.left,
                             region.top,
                             region.width,
                             region.height,
                             staging.data(),
                             rowBytes);

        shared.noteCrossing(bytes, started);
    }

    // A dispatch says nothing about where in a texture it wrote, so what it
    // owes the other side is the whole of it. The one write that does report a
    // rectangle is a host update of one, below.
    void willWriteOnComputeSide() override
    {
        makeComputeTwin();
        renderStale.add(0, 0, descriptor.width, descriptor.height);
    }

    void willWriteOnRenderSide() override
    {
        renderHasContents = true;

        if (compute != nullptr)
            computeStale.add(0, 0, descriptor.width, descriptor.height);
    }

    void update(const void* pixels, int bytesPerRow) override
    {
        if (render == nullptr)
            return;

        render->update(pixels, bytesPerRow);
        noteHostWrite(0, 0, descriptor.width, descriptor.height);
    }

    void updateRegion(int x,
                      int y,
                      int regionWidth,
                      int regionHeight,
                      const void* pixels,
                      int bytesPerRow) override
    {
        if (render == nullptr)
            return;

        render->updateRegion(x, y, regionWidth, regionHeight, pixels, bytesPerRow);
        noteHostWrite(x, y, regionWidth, regionHeight);
    }

    void noteHostWrite(int x, int y, int regionWidth, int regionHeight)
    {
        renderHasContents = true;

        if (compute == nullptr)
            return;

        renderStale.clear();
        computeStale.add(x, y, regionWidth, regionHeight);
    }

    void readRegion(int x,
                    int y,
                    int regionWidth,
                    int regionHeight,
                    void* dst,
                    int bytesPerRow) const override
    {
        if (!renderStale.isEmpty() && compute != nullptr)
        {
            shared.finishComputeWork();
            compute->readRegion(x, y, regionWidth, regionHeight, dst, bytesPerRow);
            return;
        }

        if (render != nullptr)
            render->readRegion(x, y, regionWidth, regionHeight, dst, bytesPerRow);
    }

    int width() const override { return render != nullptr ? render->width() : 0; }

    int height() const override { return render != nullptr ? render->height() : 0; }

    // The render side is the texture; a twin the compute side could not make is
    // what takes the whole thing down, since a kernel would then be binding
    // nothing.
    bool isValid() const override
    {
        if (render == nullptr || !render->isValid())
            return false;

        return !descriptor.computeWrite
               || (compute != nullptr && compute->isValid());
    }

    int mipLevels() const override
    {
        return render != nullptr ? render->mipLevels() : 0;
    }

    bool isRenderTarget() const override
    {
        return render != nullptr && render->isRenderTarget();
    }

    bool isCube() const override { return render != nullptr && render->isCube(); }

    bool isComputeWritable() const override
    {
        return compute != nullptr && compute->isComputeWritable();
    }

    bool hasDepth() const override
    {
        return render != nullptr && render->hasDepth();
    }

    bool hasStencil() const override
    {
        return render != nullptr && render->hasStencil();
    }

    bool hasSampleableDepth() const override
    {
        return render != nullptr && render->hasSampleableDepth();
    }

    int sampleCount() const override
    {
        return render != nullptr ? render->sampleCount() : 1;
    }

    void* nativeTexture() const override
    {
        auto* backend = sideNow();

        return backend != nullptr ? backend->nativeTexture() : nullptr;
    }

    void* nativeReadView() const override
    {
        auto* backend = sideNow();

        return backend != nullptr ? backend->nativeReadView() : nullptr;
    }

    // The three companions of a render target, which only the render side has.
    void* nativeDepthTexture() const override
    {
        return render != nullptr ? render->nativeDepthTexture() : nullptr;
    }

    void* nativeMultisampleTexture() const override
    {
        return render != nullptr ? render->nativeMultisampleTexture() : nullptr;
    }

    void* nativeResolvedDepthTexture() const override
    {
        return render != nullptr ? render->nativeResolvedDepthTexture() : nullptr;
    }

    CrossingResource* crossing() override { return this; }

    CompositeShared& shared;
    Device* owner = nullptr;
    TextureDescriptor descriptor;

    std::unique_ptr<TextureBackend> render;

    mutable std::unique_ptr<TextureBackend> compute;
    mutable DirtyRegion renderStale;
    mutable DirtyRegion computeStale;

    mutable bool renderHasContents = false;
    mutable std::vector<std::byte> staging;
};

CrossingResource* crossingOf(const Buffer* buffer)
{
    return buffer != nullptr ? getBufferBackend(*buffer).crossing() : nullptr;
}

CrossingResource* crossingOf(const Texture& texture)
{
    return getTextureBackend(texture).crossing();
}

// The render half of a recording. Everything it forwards runs with the side at
// Render, which is what makes each bind pull whatever a kernel left on the
// other side across before the draw reads it.
struct CompositeRenderPass final : RenderPassBackend
{
    CompositeRenderPass(CompositeShared& sharedToUse,
                        std::unique_ptr<RenderPassBackend> passToUse)
        : shared(sharedToUse)
        , pass(std::move(passToUse))
    {
    }

    int targetWidth() const override { return pass->targetWidth(); }

    int targetHeight() const override { return pass->targetHeight(); }

    void setScissorRect(const Graphics::Rect& rect) override
    {
        pass->setScissorRect(rect);
    }

    void clearScissorRect() override { pass->clearScissorRect(); }

    void setViewport(const Graphics::Rect& rect,
                     float nearDepth,
                     float farDepth) override
    {
        pass->setViewport(rect, nearDepth, farDepth);
    }

    void clearViewport() override { pass->clearViewport(); }

    void setPipeline(const RenderPipeline& pipeline) override
    {
        pass->setPipeline(pipeline);
    }

    void setStencilReference(unsigned int value) override
    {
        pass->setStencilReference(value);
    }

    void setVertexBuffer(const BufferRange& range, int index) override
    {
        SideGuard guard {shared, Side::Render};

        pass->setVertexBuffer(range, index);
    }

    void setFragmentTexture(const Texture& texture,
                            int slot,
                            TextureSampling sampling) override
    {
        SideGuard guard {shared, Side::Render};

        pass->setFragmentTexture(texture, slot, sampling);
    }

    void setFragmentDepthTexture(const Texture& renderTarget,
                                 int slot,
                                 TextureSampling sampling) override
    {
        SideGuard guard {shared, Side::Render};

        pass->setFragmentDepthTexture(renderTarget, slot, sampling);
    }

    // Refused rather than crossed: a GL with no std430 block has nothing to
    // bind this to, and copying a kernel's output the whole way over to be
    // dropped by the bind is the one crossing worth never paying (D11).
    void setStorageBuffer(const BufferRange& range, int slot) override
    {
        if (!shared.render->supportsStorageBuffers())
        {
            reportNoStorageBuffers();
            return;
        }

        SideGuard guard {shared, Side::Render};

        pass->setStorageBuffer(range, slot);
    }

    static void reportNoStorageBuffers()
    {
        static auto reported = false;

        if (reported)
            return;

        reported = true;

        LOG("Composite: the OpenGL half has no shader storage buffers, so a "
            "storage bind on a render pass is refused rather than copied "
            "across to be dropped");
    }

    void setBytes(const void* data, int bytes, int slot) override
    {
        pass->setBytes(data, bytes, slot);
    }

    void drawInstanced(int vertexCount,
                       int instanceCount,
                       int firstVertex,
                       int firstInstance) override
    {
        SideGuard guard {shared, Side::Render};

        pass->drawInstanced(vertexCount, instanceCount, firstVertex, firstInstance);
    }

    void drawIndexedInstanced(const BufferRange& indices,
                              int indexCount,
                              int instanceCount,
                              IndexFormat format,
                              int firstIndex,
                              int firstInstance,
                              int baseVertex) override
    {
        SideGuard guard {shared, Side::Render};

        pass->drawIndexedInstanced(indices,
                                   indexCount,
                                   instanceCount,
                                   format,
                                   firstIndex,
                                   firstInstance,
                                   baseVertex);
    }

    void end() override { pass->end(); }

    CompositeShared& shared;
    std::unique_ptr<RenderPassBackend> pass;
};

// The compute half of one. The side stays raised for the whole pass, so every
// resource it binds hands over its Vulkan twin - making one and filling it from
// the render side where this is the first kernel to ask.
struct CompositeComputePass final : ComputePassBackend
{
    CompositeComputePass(CompositeShared& sharedToUse,
                         std::unique_ptr<ComputePassBackend> passToUse)
        : shared(sharedToUse)
        , pass(std::move(passToUse))
        , guard(sharedToUse, Side::Compute)
    {
    }

    bool setPipeline(const ComputePipeline& pipeline) override
    {
        return pass->setPipeline(pipeline);
    }

    void setInputBuffer(const BufferRange& range, int slot) override
    {
        pass->setInputBuffer(range, slot);
    }

    void setOutputBuffer(const BufferRange& range, int slot) override
    {
        if (auto* resource = crossingOf(range.buffer))
            resource->willWriteOnComputeSide();

        pass->setOutputBuffer(range, slot);
    }

    void setInputTexture(const Texture& texture,
                         int slot,
                         TextureSampling sampling) override
    {
        pass->setInputTexture(texture, slot, sampling);
    }

    void setOutputTexture(const Texture& texture, int slot) override
    {
        if (auto* resource = crossingOf(texture))
            resource->willWriteOnComputeSide();

        pass->setOutputTexture(texture, slot);
    }

    void setBytes(const void* data, std::int64_t bytes, int slot) override
    {
        pass->setBytes(data, bytes, slot);
    }

    void dispatch(int width, int height, int depth, ThreadGroupShape group) override
    {
        pass->dispatch(width, height, depth, group);
    }

    void dispatchIndirect(const Buffer& arguments,
                          std::int64_t offsetInBytes) override
    {
        pass->dispatchIndirect(arguments, offsetInBytes);
    }

    void barrier() override { pass->barrier(); }

    void end() override { pass->end(); }

    CompositeShared& shared;
    std::unique_ptr<ComputePassBackend> pass;

    // Last, so it is released after the pass it was raised for has ended.
    SideGuard guard;
};

// A recording on the compute half: opened by whichever of a Frame and a
// CommandBuffer wanted a kernel, submitted and waited for the moment anything
// on the render half wants what it wrote.
struct ComputeRecording
{
    ComputeRecording(CompositeShared& sharedToUse, Device& device)
        : shared(sharedToUse)
        , commands(sharedToUse.compute->makeCommandBuffer(device))
    {
    }

    ~ComputeRecording() { finish(); }

    ComputeRecording(const ComputeRecording&) = delete;
    ComputeRecording& operator=(const ComputeRecording&) = delete;

    // Listed as open the moment anything is recorded onto it, so that whatever
    // wants its results knows there is something to wait for.
    void noteRecorded()
    {
        if (recorded)
            return;

        recorded = true;
        shared.openRecordings.push_back(this);
    }

    std::unique_ptr<ComputePassBackend> beginCompute(std::string_view label,
                                                     DispatchOrder order)
    {
        if (commands == nullptr)
            return nullptr;

        SideGuard guard {shared, Side::Compute};

        auto pass = commands->beginCompute(label, order);

        if (pass == nullptr)
            return nullptr;

        noteRecorded();

        return pass;
    }

    // Submit is a no-op on a recording already committed and wait is one on a
    // recording already finished, so this is safe however the caller got here.
    void finish()
    {
        if (!recorded)
            return;

        recorded = false;

        auto& open = shared.openRecordings;
        open.erase(std::remove(open.begin(), open.end(), this), open.end());

        SideGuard guard {shared, Side::Compute};

        commands->submit();
        commands->wait();
    }

    CompositeShared& shared;
    std::unique_ptr<CommandBufferBackend> commands;
    bool recorded = false;
};

void CompositeShared::finishComputeWork()
{
    while (!openRecordings.empty())
        openRecordings.back()->finish();
}

// A frame is the render half's, with a compute recording beside it for the
// kernels it opens. beginPass finishes that recording before the first draw,
// which is when everything a dispatch wrote crosses (D11).
struct CompositeFrame final : FrameBackend
{
    CompositeFrame(CompositeShared& sharedToUse,
                   Device& device,
                   std::unique_ptr<FrameBackend> frameToUse)
        : shared(sharedToUse)
        , owner(&device)
        , frame(std::move(frameToUse))
    {
    }

    ~CompositeFrame() override { compute.reset(); }

    bool isValid() const override { return frame->isValid(); }

    Graphics::Point pixelSize() const override { return frame->pixelSize(); }

    void beginTiming() override { frame->beginTiming(); }

    void flush() override
    {
        finishCompute();
        frame->flush();
    }

    void finishCompute()
    {
        if (compute != nullptr)
            compute->finish();
    }

    std::unique_ptr<RenderPassBackend> wrap(std::unique_ptr<RenderPassBackend> pass)
    {
        if (pass == nullptr)
            return nullptr;

        return std::make_unique<CompositeRenderPass>(shared, std::move(pass));
    }

    std::unique_ptr<RenderPassBackend>
        beginPass(const RenderPassDescriptor& descriptor) override
    {
        finishCompute();

        return wrap(frame->beginPass(descriptor));
    }

    std::unique_ptr<RenderPassBackend>
        beginPass(const Texture& target,
                  const RenderPassDescriptor& descriptor) override
    {
        finishCompute();

        if (auto* resource = crossingOf(target))
            resource->willWriteOnRenderSide();

        return wrap(frame->beginPass(target, descriptor));
    }

    std::unique_ptr<ComputePassBackend> beginCompute(std::string_view label,
                                                     DispatchOrder order) override
    {
        if (compute == nullptr)
            compute = std::make_unique<ComputeRecording>(shared, *owner);

        auto pass = compute->beginCompute(label, order);

        if (pass == nullptr)
            return nullptr;

        return std::make_unique<CompositeComputePass>(shared, std::move(pass));
    }

    CompositeShared& shared;
    Device* owner = nullptr;

    std::unique_ptr<FrameBackend> frame;
    std::unique_ptr<ComputeRecording> compute;
};

// A CommandBuffer is the compute half's whole and entire: there is no frame
// around it and nothing it records draws. Its timings are the compute clock,
// which is what keeps the two from being reported as a sum.
struct CompositeCommandBuffer final : CommandBufferBackend
{
    CompositeCommandBuffer(CompositeShared& sharedToUse, Device& device)
        : shared(sharedToUse)
        , recording(sharedToUse, device)
    {
    }

    bool isValid() const override
    {
        return recording.commands != nullptr && recording.commands->isValid();
    }

    std::unique_ptr<ComputePassBackend> beginCompute(std::string_view label,
                                                     DispatchOrder order) override
    {
        auto pass = recording.beginCompute(label, order);

        if (pass == nullptr)
            return nullptr;

        return std::make_unique<CompositeComputePass>(shared, std::move(pass));
    }

    void fill(const BufferRange& range, std::uint8_t value) override
    {
        if (auto* resource = crossingOf(range.buffer))
            resource->willWriteOnComputeSide();

        SideGuard guard {shared, Side::Compute};

        recording.noteRecorded();
        recording.commands->fill(range, value);
    }

    void submit() override
    {
        SideGuard guard {shared, Side::Compute};

        recording.commands->submit();
    }

    void wait() override
    {
        SideGuard guard {shared, Side::Compute};

        recording.commands->wait();
    }

    bool isComplete() const override { return recording.commands->isComplete(); }

    Threads::Async<void> commitAsync() override
    {
        SideGuard guard {shared, Side::Compute};

        return recording.commands->commitAsync();
    }

    const FrameTimings& timings() override { return recording.commands->timings(); }

    bool supportsPassTimings() const override
    {
        return recording.commands->supportsPassTimings();
    }

    CompositeShared& shared;
    ComputeRecording recording;
};

// Two clocks, one per side, and never a sum: a timer belongs to one half for
// its whole life, and which half is settled by the first recording it is given
// - a VkCommandBuffer from the compute side's CommandTimer, and nothing at all
// from the render side's FrameTimer.
struct CompositeTimestamps final : GpuTimestampsBackend
{
    CompositeTimestamps(std::unique_ptr<GpuTimestampsBackend> renderToUse,
                        std::unique_ptr<GpuTimestampsBackend> computeToUse)
        : render(std::move(renderToUse))
        , compute(std::move(computeToUse))
    {
    }

    GpuTimestampsBackend& chosen() const
    {
        return onComputeSide ? *compute : *render;
    }

    bool isSupported() const override { return chosen().isSupported(); }

    void beginSlot(int slot, Device& device) override
    {
        render->beginSlot(slot, device);
        compute->beginSlot(slot, device);
    }

    void beginRecording(int slot, void* nativeCommandBuffer) override
    {
        onComputeSide = nativeCommandBuffer != nullptr;

        chosen().beginRecording(slot, nativeCommandBuffer);
    }

    void* nativeSamples(int slot) const override
    {
        return chosen().nativeSamples(slot);
    }

    bool endSlot(int slot, int passCount, void* nativeCommandBuffer) override
    {
        return chosen().endSlot(slot, passCount, nativeCommandBuffer);
    }

    void noteSubmitted(int slot, std::uint64_t fenceValue) override
    {
        chosen().noteSubmitted(slot, fenceValue);
    }

    bool isSlotComplete(int slot, const Device& device) const override
    {
        return chosen().isSlotComplete(slot, device);
    }

    double resolveSlot(int slot, int passCount, double* milliseconds) override
    {
        return chosen().resolveSlot(slot, passCount, milliseconds);
    }

    std::unique_ptr<GpuTimestampsBackend> render;
    std::unique_ptr<GpuTimestampsBackend> compute;

    bool onComputeSide = false;
};

struct CompositeDeviceBackend final : DeviceBackend
{
    CompositeDeviceBackend()
    {
        shared.render = makeGLDeviceBackend();
        shared.compute = makeVulkanDeviceBackend();
    }

    std::string backendName() const override { return "OpenGL+Vulkan"; }

    DeviceBackend* sideFor(GPUApi api) override
    {
        return api == GPUApi::Vulkan ? shared.compute.get() : shared.render.get();
    }

    // The render half is the device: a composite with no OpenGL has nothing to
    // draw with, and one with no Vulkan is simply a Device without kernels.
    bool isValid() const override { return shared.render->isValid(); }

    std::string name() const override
    {
        if (!isValid())
            return "no OpenGL device";

        if (!supportsCompute())
            return shared.render->name();

        return shared.render->name() + " + " + shared.compute->name();
    }

    // Which is the whole point of it.
    bool supportsCompute() const override
    {
        return shared.compute->isValid() && shared.compute->supportsCompute();
    }

    // The render side's, because this is what a caller with a second route asks
    // before it binds one to a draw. A kernel's own storage buffers are the
    // compute side's and are never in question - every Vulkan device has them -
    // so a false here takes nothing away from a dispatch.
    bool supportsStorageBuffers() const override
    {
        return shared.render->supportsStorageBuffers();
    }

    // Both of these are read of a render target, so both are the render side's.
    bool supportsZeroToOneDepth() const override
    {
        return shared.render->supportsZeroToOneDepth();
    }

    bool supportsSampleCount(int count) const override
    {
        return shared.render->supportsSampleCount(count);
    }

    bool supportsBlockCompression() const override
    {
        return shared.render->supportsBlockCompression();
    }

    // The larger of the two, since a range a caller sub-allocates may be bound
    // on either side and one number has to satisfy both.
    int storageBufferOffsetAlignment() const override
    {
        const auto render = shared.render->storageBufferOffsetAlignment();
        const auto compute =
            supportsCompute() ? shared.compute->storageBufferOffsetAlignment() : 4;

        return std::max(render, compute);
    }

    // The compute side's: it is the budget a kernel's shared<> arrays are spent
    // out of, and the kernels are Vulkan's here.
    int maxThreadgroupMemory() const override
    {
        return supportsCompute() ? shared.compute->maxThreadgroupMemory() : 0;
    }

    bool supportsHalfSimdMatrix() const override { return false; }

    bool supportsBFloat16SimdMatrix() const override { return false; }

    // Nothing asks: getVulkanContext and getGLContext go through sideFor, which
    // is what a composite Device made necessary.
    void* nativeContext() const override { return nullptr; }

    // The compute side's, both of them, because they are the only answers that
    // mean here what they mean on every other backend: OpenGL has no queue at
    // all, and what it would offer as a device is an EGLContext - a context per
    // Device over one driver, rather than the driver itself.
    void* nativeDevice() const override { return shared.compute->nativeDevice(); }

    void* nativeQueue() const override { return shared.compute->nativeQueue(); }

    void* nativeTextureCache() const override { return nullptr; }

    // This one does follow the side: a GL render pass looks a sampler object up
    // through it, and a Vulkan compute pass its VkSampler.
    void* nativeSampler(TextureSampling sampling) const override
    {
        auto& backend =
            shared.side == Side::Compute ? *shared.compute : *shared.render;

        return backend.nativeSampler(sampling);
    }

    void followMainThread() override
    {
        shared.render->followMainThread();
        shared.compute->followMainThread();
    }

    void waitForSubmittedWork() override
    {
        shared.finishComputeWork();
        shared.compute->waitForSubmittedWork();
        shared.render->waitForSubmittedWork();
    }

    void noteOwner(Device& device)
    {
        if (shared.owner == nullptr)
            shared.owner = &device;
    }

    std::unique_ptr<BufferBackend> makeBuffer(Device& device,
                                              const void* data,
                                              std::int64_t bytes,
                                              BufferUsage usage,
                                              BufferStorage storage) override
    {
        noteOwner(device);

        return std::make_unique<CompositeBuffer>(
            shared, device, data, bytes, usage, storage);
    }

    std::unique_ptr<TextureBackend> makeTexture(Device& device,
                                                const TextureDescriptor& descriptor,
                                                const void* pixels) override
    {
        noteOwner(device);

        return std::make_unique<CompositeTexture>(
            shared, device, descriptor, pixels);
    }

    std::unique_ptr<TextureBackend> wrapPixelBuffer(Device& device,
                                                    void* nativePixelBuffer) override
    {
        return shared.render->wrapPixelBuffer(device, nativePixelBuffer);
    }

    // Which side a library belongs to is the source's own answer, so neither is
    // ever compiled for the half that would refuse it.
    std::unique_ptr<ShaderLibraryBackend>
        makeShaderLibrary(Device& device, const ShaderSource& source) override
    {
        if (!source.isCompute())
            return shared.render->makeShaderLibrary(device, source);

        SideGuard guard {shared, Side::Compute};

        return shared.compute->makeShaderLibrary(device, source);
    }

    std::unique_ptr<RenderPipelineBackend>
        makeRenderPipeline(Device& device,
                           const RenderPipelineDescriptor& descriptor) override
    {
        return shared.render->makeRenderPipeline(device, descriptor);
    }

    std::unique_ptr<ComputePipelineBackend>
        makeComputePipeline(Device& device, const ShaderLibrary& library) override
    {
        SideGuard guard {shared, Side::Compute};

        return shared.compute->makeComputePipeline(device, library);
    }

    std::unique_ptr<CommandBufferBackend> makeCommandBuffer(Device& device) override
    {
        noteOwner(device);

        SideGuard guard {shared, Side::Compute};

        return std::make_unique<CompositeCommandBuffer>(shared, device);
    }

    std::unique_ptr<FrameBackend> makeFrame(Device& device, void* drawable) override
    {
        noteOwner(device);

        return std::make_unique<CompositeFrame>(
            shared, device, shared.render->makeFrame(device, drawable));
    }

    std::unique_ptr<FrameBackend> makeFrame(Device& device,
                                            const OffscreenTarget& target) override
    {
        noteOwner(device);

        return std::make_unique<CompositeFrame>(
            shared, device, shared.render->makeFrame(device, target));
    }

    std::unique_ptr<GpuTimestampsBackend> makeGpuTimestamps() override
    {
        return std::make_unique<CompositeTimestamps>(
            shared.render->makeGpuTimestamps(), shared.compute->makeGpuTimestamps());
    }

    std::unique_ptr<GPUViewBackend>
        makeGPUView(GPUView& view, Graphics::ViewSurface& record) override
    {
        return shared.render->makeGPUView(view, record);
    }

    mutable CompositeShared shared;
};
} // namespace

std::unique_ptr<DeviceBackend> makeCompositeDeviceBackend()
{
    return std::make_unique<CompositeDeviceBackend>();
}
} // namespace eacp::GPU
