#pragma once

#include <eacp/Core/Threads/Timer.h>
#include <eacp/Core/Utils/Containers.h>
#include <eacp/Core/Utils/WinInclude.h>

#include <d3d12.h>
#include <dxgi1_4.h>

#include <winrt/base.h>

#include <cstddef>
#include <cstdint>
#include <optional>

// Process-wide D3D12 plumbing shared by every Windows GPU translation unit:
// the device and direct queue, the fence that orders CPU/GPU work, a pool of
// command allocator/list pairs recycled once their fence value passes, and the
// shader-visible descriptor heaps textures allocate their SRV/sampler slots
// from. The 2D graphics layer keeps its own D3D11 device for Direct2D; the two
// stacks only meet in the compositor, which composes swapchains from either
// device. Not part of GPU.h.

namespace eacp::GPU
{
// One recording in flight: an allocator/list pair plus the transient upload
// resources (per-draw constant buffers, staging copies) the recorded commands
// reference. Everything is released together once fenceValue has passed.
struct CommandContext
{
    // Where everything this recording uploads from the CPU is bump-allocated:
    // an upload-heap buffer left mapped for its whole life, handed out a range
    // at a time. Root constants read straight out of it, and a buffer's
    // CopyBufferRegion sources from it.
    //
    // One resource per *recording* rather than per upload, which is the whole
    // point. A committed resource costs a quarter of a millisecond to create on
    // some adapters, and a frame uploads a couple of hundred times - every
    // uniform set and every batch flush - so creating one each was by far the
    // most expensive thing a frame did.
    //
    // A chain rather than one buffer because a recording that wants more than the
    // chunk holds cannot be given a larger one: the commands already recorded
    // point into the old resource. So a second chunk is added and both are kept,
    // which also means the chain settles at the peak a frame actually uses and
    // then stops growing.
    struct UploadChunk
    {
        winrt::com_ptr<ID3D12Resource> resource;
        std::uint8_t* mapped = nullptr;
        std::size_t capacity = 0;
        std::size_t used = 0;
    };

    // Ready to be filled from the start again. Only sound once the recording's
    // fence has passed, which is the same condition that lets the allocator be
    // reset -- the GPU has read everything these chunks carried.
    void rewindUploads()
    {
        for (auto& chunk: uploads)
            chunk.used = 0;

        uploadCursor = 0;
    }

    winrt::com_ptr<ID3D12CommandAllocator> allocator;
    winrt::com_ptr<ID3D12GraphicsCommandList> list;
    Vector<winrt::com_ptr<ID3D12Resource>> transients;
    std::uint64_t fenceValue = 0;

    Vector<UploadChunk> uploads;
    int uploadCursor = 0;

    // Staging-pool slots this recording is copying out of. Unlike transients
    // these are not released when the recording is recycled — they go back to
    // the pool, stamped at submit with the fence that frees them again.
    Vector<int> stagingTaken;

    // Identifies the recording for buffer state tracking: a buffer first
    // touched under a new id was implicitly promoted from COMMON, so no
    // barrier is needed (buffers decay back to COMMON after every execute).
    std::uint64_t recordingId = 0;
};

// A range of a recording's upload arena: what a copy sources from, where the
// CPU writes, and the address a root descriptor binds to. Valid until the
// recording it came from has completed on the GPU.
struct UploadRange
{
    bool isValid() const { return mapped != nullptr; }

    ID3D12Resource* resource = nullptr;
    std::uint8_t* mapped = nullptr;
    std::size_t offset = 0;
    D3D12_GPU_VIRTUAL_ADDRESS address = 0;
};

// A slot in one of the shader-visible heaps. The generation guards frees that
// arrive after device loss rebuilt the heaps: a stale slot is simply ignored.
struct DescriptorSlot
{
    UINT index = 0;
    std::uint64_t generation = 0;
    D3D12_CPU_DESCRIPTOR_HANDLE cpu = {};
    D3D12_GPU_DESCRIPTOR_HANDLE gpu = {};
};

class D3D12Context
{
public:
    D3D12Context();
    ~D3D12Context();

    bool isValid() const { return device != nullptr; }

    ID3D12Device* getDevice() const { return device.get(); }
    ID3D12CommandQueue* getQueue() const { return queue.get(); }
    ID3D12RootSignature* getRenderRootSignature() const
    {
        return renderRootSignature.get();
    }
    ID3D12RootSignature* getComputeRootSignature() const
    {
        return computeRootSignature.get();
    }

    // The command signature ExecuteIndirect needs to read a dispatch out of a
    // buffer. One argument, of type Dispatch, which is the case D3D12 lets a
    // signature carry a null root signature for: nothing about the bindings
    // changes per command, only the grid. Built on first use and shared, since
    // it depends on nothing but the device.
    ID3D12CommandSignature* getDispatchSignature();
    ID3D12DescriptorHeap* getTextureHeap() const
    {
        return textureDescriptors.heap.get();
    }
    ID3D12DescriptorHeap* getSamplerHeap() const
    {
        return samplerDescriptors.heap.get();
    }

    // Permanently valid descriptors for the table slots a shader leaves unused,
    // which Tier 1 hardware still requires to be bound. See
    // createNullDescriptors.
    D3D12_GPU_DESCRIPTOR_HANDLE getNullTextureDescriptor() const
    {
        return nullTexture.gpu;
    }

    // Its write-side sibling, for the compute signature's UAV tables. A table
    // declared as a UAV range will not take an SRV descriptor, so the two
    // cannot share one.
    D3D12_GPU_DESCRIPTOR_HANDLE getNullTextureUAVDescriptor() const
    {
        return nullTextureUAV.gpu;
    }
    D3D12_GPU_DESCRIPTOR_HANDLE getNullSamplerDescriptor() const
    {
        return nullSampler.gpu;
    }

    // An open command list ready for recording. Owned by the caller until it
    // is handed back through submit() or discard().
    CommandContext* acquire();

    // The recording a frame currently has open, or null outside one. A CPU
    // upload that happens while a frame is being recorded puts its copy on this
    // list instead of acquiring and submitting one of its own: same queue, in
    // order ahead of the draw or dispatch that wanted the bytes, and no
    // submission per buffer.
    //
    // Set by Frame for as long as it is recording, which is what makes it safe
    // for an upload to assume the list is still open and still ahead of the work
    // that reads it. The one thing it is not safe for is a read-back of bytes
    // uploaded this way before the frame submits -- see Buffer::read.
    void setOpenRecording(CommandContext* commands) { openRecording = commands; }
    CommandContext* getOpenRecording() const { return openRecording; }

    // Closes and executes the list, signals the fence and recycles the
    // context. Returns the fence value that completes when the GPU finishes.
    std::uint64_t submit(CommandContext* commands);

    // Recycles a recording that should never reach the GPU.
    void discard(CommandContext* commands);

    std::uint64_t lastSubmitted() const { return lastSubmittedValue; }
    bool hasCompleted(std::uint64_t value) const;
    void waitFor(std::uint64_t value);
    void waitIdle();

    // Calls `done` once `value` has passed on the GPU, without blocking — the
    // non-blocking sibling of waitFor, and what CommandBuffer::commitAsync is
    // built on. Fires inline when the value has already passed; otherwise from
    // a poll on the event loop, so `done` always runs on the main thread.
    //
    // A poll rather than a waiter thread because the whole backend is
    // main-thread only: a fence event would need a thread whose only job is to
    // hand the result straight back here. The cost is a completion latency of
    // up to one poll interval, against a dispatch measured in milliseconds.
    void notifyWhenCompleted(std::uint64_t value, Callback done);

    // Copies bytes into the recording's upload arena (so they outlive GPU
    // execution) and returns their address for a root CBV. Returns 0 on
    // failure.
    D3D12_GPU_VIRTUAL_ADDRESS uploadConstants(CommandContext& commands,
                                              const void* data,
                                              std::size_t bytes);

    // Room for `bytes` on the recording's upload arena, for a caller that wants
    // to copy out of it rather than have the GPU read it in place — the source
    // of a buffer or texture upload. The CPU writes through `mapped` and the
    // copy sources from `resource` at `offset`. Invalid on failure.
    //
    // Not acquireStagingBuffer, which lends whole pooled resources and is right
    // for the handful of very large uploads a video frame makes. A frame of a
    // component interface makes hundreds of small ones, all of which must hold
    // their bytes until the one recording carrying them submits, so a pool would
    // need a slot per upload — measured at 2068 committed resources on the frame
    // a path-heavy interface builds its masks.
    UploadRange allocateUpload(CommandContext& commands, std::size_t bytes);

    // A standalone upload-heap buffer pre-filled with the bytes, for staging
    // resource initial data. The caller parks it on a recording.
    winrt::com_ptr<ID3D12Resource> makeUploadBuffer(const void* data,
                                                    std::size_t bytes);

    // An upload-heap buffer of at least `bytes`, borrowed from a pool and
    // returned once `commands` completes on the GPU. Null on failure.
    //
    // For staging that repeats every frame at a size worth pooling — a video
    // frame is a 33 MB upload at 4K and 133 MB at 8K — where creating and
    // destroying a committed resource that large per frame costs considerably
    // more than the copy it exists for. The caller must not park the result in
    // transients; the pool owns it.
    ID3D12Resource* acquireStagingBuffer(CommandContext& commands,
                                         std::size_t bytes);

    // A default-heap buffer of at least `bytes` with these flags: one the GPU has
    // finished with when there is a spare, and a fresh committed resource
    // otherwise. Handed back through recycleDefaultBuffer.
    //
    // This exists because a buffer has to be *replaced* rather than refilled to
    // stay correct -- two draws in one frame each read their own instances, so
    // the second cannot overwrite what the first is going to read (see
    // ShaderProgram::uploadIndices). Replacing it is therefore the hot path, and
    // a committed resource is far too expensive to create per draw. Reusing the
    // resource underneath keeps the semantics and drops the cost.
    //
    // Only for a buffer that arrives with data to fill it. A fresh committed
    // resource is zero-filled by the runtime and a recycled one holds whatever
    // was last in it, so a buffer created empty -- a compute output target --
    // must not be given one.
    winrt::com_ptr<ID3D12Resource> takeDefaultBuffer(std::size_t bytes,
                                                     D3D12_RESOURCE_FLAGS flags);

    // Offers a default-heap buffer up for reuse. Held until its fence has
    // passed, on the same terms as deferRelease, and released rather than kept
    // if the free list is already carrying enough.
    void recycleDefaultBuffer(winrt::com_ptr<ID3D12Resource> resource,
                              std::size_t capacity,
                              D3D12_RESOURCE_FLAGS flags);

    DescriptorSlot allocateTextureDescriptor();
    void freeTextureDescriptor(const DescriptorSlot& slot);
    DescriptorSlot allocateSamplerDescriptor();
    void freeSamplerDescriptor(const DescriptorSlot& slot);

    // Keeps an object alive until the GPU finished all work submitted so far
    // and no recording is still open. D3D11's bind ref-counting did this
    // implicitly; buffer, texture and pipeline destructors route their objects
    // through here instead of releasing something still in flight.
    //
    // Templated rather than fixed to ID3D12Resource because a command list
    // references a pipeline state exactly as it does a resource, and a renderer
    // constructed inside render() destroys its PSO while that list is open.
    template <typename T>
    void deferRelease(winrt::com_ptr<T> object)
    {
        if (object == nullptr)
            return;

        deferReleaseUnknown(object.template as<IUnknown>());
    }

    // Tears everything down and rebuilds on a fresh device after device
    // removal. Resources created on the old device (app buffers, textures,
    // pipelines) stay dead; their owners rebuild via GPUView::onDeviceRestored,
    // mirroring the D3D11 backend's recovery contract.
    void recreateAfterDeviceLoss();

private:
    struct DescriptorAllocator
    {
        winrt::com_ptr<ID3D12DescriptorHeap> heap;
        Vector<UINT> freeList;
        UINT next = 0;
        UINT capacity = 0;
        UINT descriptorSize = 0;
    };

    void createAll();
    void createDevice();
    void createRootSignatures();
    void createNullDescriptors();
    DescriptorAllocator makeDescriptorAllocator(D3D12_DESCRIPTOR_HEAP_TYPE type,
                                                UINT capacity);
    DescriptorSlot allocateFrom(DescriptorAllocator& allocator);
    void freeFrom(DescriptorAllocator& allocator, const DescriptorSlot& slot);
    std::uint64_t signal();

    // A chunk of this recording's upload arena with room for `bytes`, adding one
    // to the chain if nothing already there has it. Null on failure.
    CommandContext::UploadChunk* uploadRoomFor(CommandContext& commands,
                                               std::size_t bytes);

    void purgeRetired();
    void releaseRecycledBuffers();
    void pollCompletions();
    void deferReleaseUnknown(winrt::com_ptr<IUnknown> object);

    // Hands a recording's staging slots back to the pool. `freeAt` is the fence
    // value that must pass before they can be lent out again — 0 for a
    // recording that never reached the GPU, so its slots are free at once.
    void returnStaging(CommandContext& commands, std::uint64_t freeAt);

    winrt::com_ptr<ID3D12Device> device;
    winrt::com_ptr<ID3D12CommandQueue> queue;
    winrt::com_ptr<ID3D12Fence> fence;
    HANDLE fenceEvent = nullptr;
    std::uint64_t nextFenceValue = 1;
    std::uint64_t lastSubmittedValue = 0;
    std::uint64_t recordingCounter = 0;
    std::uint64_t generation = 1;

    winrt::com_ptr<ID3D12RootSignature> renderRootSignature;
    winrt::com_ptr<ID3D12RootSignature> computeRootSignature;
    winrt::com_ptr<ID3D12CommandSignature> dispatchSignature;

    DescriptorAllocator textureDescriptors;
    DescriptorAllocator samplerDescriptors;

    // Allocated once in createNullDescriptors and deliberately never freed.
    DescriptorSlot nullTexture;
    DescriptorSlot nullTextureUAV;
    DescriptorSlot nullSampler;

    OwnedVector<CommandContext> pool;
    Vector<CommandContext*> available;
    CommandContext* openRecording = nullptr;

    // An object whose owner is gone but which a command list may still
    // reference. `stamped` marks the ones a fence value has been worked out for;
    // until then there is no bound on when they are free. See purgeRetired.
    struct Retired
    {
        winrt::com_ptr<IUnknown> object;
        std::uint64_t fenceValue = 0;
        bool stamped = false;
    };

    Vector<Retired> retired;

    // A default-heap buffer offered back for reuse. `stamped` and `fenceValue`
    // work exactly as Retired's do -- the resource cannot be handed out again
    // until every list that named it has completed.
    struct PooledBuffer
    {
        winrt::com_ptr<ID3D12Resource> resource;
        std::size_t capacity = 0;
        D3D12_RESOURCE_FLAGS flags = D3D12_RESOURCE_FLAG_NONE;
        std::uint64_t fenceValue = 0;
        bool stamped = false;
    };

    // How much the free list will hold. Past it a returned buffer is released
    // instead of kept, so an interface that allocates in a burst and then
    // settles does not carry the burst's worth of memory for the rest of the run.
    static constexpr std::size_t reusableBudget = 32 * 1024 * 1024;

    Vector<PooledBuffer> recycling;
    Vector<PooledBuffer> reusable;
    std::size_t reusableBytes = 0;

    // Upload-heap buffers kept for reuse. `freeAt` is the fence value that must
    // pass before a slot can be lent out again; `lent` marks the window between
    // acquireStagingBuffer and the submit that stamps the real fence, during
    // which the slot must not be handed to a second recording.
    struct StagingBuffer
    {
        winrt::com_ptr<ID3D12Resource> resource;
        std::size_t bytes = 0;
        std::uint64_t freeAt = 0;
        bool lent = false;
    };

    Vector<StagingBuffer> staging;

    // Callbacks owed to submissions still running, and the poll that settles
    // them. The timer only exists while something is pending, so an app that
    // never calls commitAsync never pays for it.
    struct PendingCompletion
    {
        std::uint64_t fenceValue = 0;
        Callback done;
    };

    static constexpr int completionPollHz = 240;

    Vector<PendingCompletion> pendingCompletions;
    std::optional<Threads::Timer> completionPoll;
};

// The process-wide context, created on first use. Main-thread only, like the
// rest of the GPU backend.
D3D12Context& getD3D12Context();
} // namespace eacp::GPU
