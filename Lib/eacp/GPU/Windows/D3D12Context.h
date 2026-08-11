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

// Process-wide D3D12 plumbing shared by every Windows GPU translation unit.
// The 2D graphics layer keeps its own D3D11 device; the two stacks meet only in
// the compositor. Not part of GPU.h.

namespace eacp::GPU
{
// One recording in flight: an allocator/list pair plus the transient upload
// resources the recorded commands reference, all released once fenceValue
// passes.
struct CommandContext
{
    // A permanently mapped upload-heap buffer, bump-allocated a range at a time:
    // one resource per recording, not per upload. A chain rather than one
    // buffer, already-recorded commands pointing into the old resource.
    struct UploadChunk
    {
        winrt::com_ptr<ID3D12Resource> resource;
        std::uint8_t* mapped = nullptr;
        std::size_t capacity = 0;
        std::size_t used = 0;
    };

    // Only sound once the recording's fence has passed, the same condition that
    // lets the allocator be reset.
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

    // Unlike transients these go back to the staging pool when the recording is
    // recycled, stamped at submit with the fence that frees them.
    Vector<int> stagingTaken;

    // Identifies the recording for buffer state tracking: a buffer first touched
    // under a new id was implicitly promoted from COMMON and needs no barrier.
    std::uint64_t recordingId = 0;
};

// A range of a recording's upload arena, valid until that recording has
// completed on the GPU.
struct UploadRange
{
    bool isValid() const { return mapped != nullptr; }

    ID3D12Resource* resource = nullptr;
    std::uint8_t* mapped = nullptr;
    std::size_t offset = 0;
    D3D12_GPU_VIRTUAL_ADDRESS address = 0;
};

// A slot in a shader-visible heap. The generation guards frees arriving after
// device loss rebuilt the heaps: a stale slot is ignored.
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

    // For ExecuteIndirect. One Dispatch argument, which is the case D3D12 lets a
    // signature carry a null root signature for. Built on first use.
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
    // which Tier 1 hardware still requires bound. See createNullDescriptors.
    D3D12_GPU_DESCRIPTOR_HANDLE getNullTextureDescriptor() const
    {
        return nullTexture.gpu;
    }

    // A UAV range will not take an SRV descriptor, so the two cannot share one.
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

    // The recording a frame has open, or null outside one. CPU uploads join it
    // instead of submitting per buffer, landing in order ahead of the work that
    // reads them - but not readable before the frame submits. See Buffer::read.
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

    // Non-blocking sibling of waitFor. Fires inline if `value` already passed,
    // otherwise from an event-loop poll, so `done` always runs on the main
    // thread - the backend being main-thread only.
    void notifyWhenCompleted(std::uint64_t value, Callback done);

    // Copies bytes into the recording's upload arena (so they outlive GPU
    // execution) and returns their address for a root CBV. Returns 0 on
    // failure.
    D3D12_GPU_VIRTUAL_ADDRESS uploadConstants(CommandContext& commands,
                                              const void* data,
                                              std::size_t bytes);

    // Room for `bytes` on the recording's upload arena; the CPU writes through
    // `mapped` and the copy sources from `resource` at `offset`. For the many
    // small uploads a frame makes; acquireStagingBuffer would need a slot each.
    UploadRange allocateUpload(CommandContext& commands, std::size_t bytes);

    // A standalone upload-heap buffer pre-filled with the bytes, for staging
    // resource initial data. The caller parks it on a recording.
    winrt::com_ptr<ID3D12Resource> makeUploadBuffer(const void* data,
                                                    std::size_t bytes);

    // An upload-heap buffer of at least `bytes`, borrowed from the pool and
    // returned once `commands` completes; for the few very large per-frame
    // uploads. The pool owns it, so do not park it in transients.
    ID3D12Resource* acquireStagingBuffer(CommandContext& commands,
                                         std::size_t bytes);

    // A spare the GPU has finished with, or a fresh committed resource; handed
    // back through recycleDefaultBuffer. Only for a buffer arriving with data to
    // fill it, a recycled one holding whatever was last in it.
    winrt::com_ptr<ID3D12Resource> takeDefaultBuffer(std::size_t bytes,
                                                     D3D12_RESOURCE_FLAGS flags);

    // Held until its fence has passed, on the same terms as deferRelease, and
    // released rather than kept once the free list is carrying enough.
    void recycleDefaultBuffer(winrt::com_ptr<ID3D12Resource> resource,
                              std::size_t capacity,
                              D3D12_RESOURCE_FLAGS flags);

    DescriptorSlot allocateTextureDescriptor();
    void freeTextureDescriptor(const DescriptorSlot& slot);
    DescriptorSlot allocateSamplerDescriptor();
    void freeSamplerDescriptor(const DescriptorSlot& slot);

    // Keeps an object alive until the GPU finished all submitted work and no
    // recording is open. Resource, texture and pipeline destructors route
    // through here rather than releasing something a command list still names.
    template <typename T>
    void deferRelease(winrt::com_ptr<T> object)
    {
        if (object == nullptr)
            return;

        deferReleaseUnknown(object.template as<IUnknown>());
    }

    // Resources created on the old device stay dead; their owners rebuild via
    // GPUView::onDeviceRestored.
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

    // Adds a chunk to the chain if nothing already there fits. Null on failure.
    CommandContext::UploadChunk* uploadRoomFor(CommandContext& commands,
                                               std::size_t bytes);

    void purgeRetired();
    void releaseRecycledBuffers();
    void pollCompletions();
    void deferReleaseUnknown(winrt::com_ptr<IUnknown> object);

    // `freeAt` is the fence value that must pass before the slots can be lent
    // again; 0 for a recording that never reached the GPU.
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

    // An object whose owner is gone but which a command list may still name.
    // `stamped` marks those a fence value is known for. See purgeRetired.
    struct Retired
    {
        winrt::com_ptr<IUnknown> object;
        std::uint64_t fenceValue = 0;
        bool stamped = false;
    };

    Vector<Retired> retired;

    // `stamped` and `fenceValue` work as Retired's do: the resource cannot be
    // handed out again until every list that named it has completed.
    struct PooledBuffer
    {
        winrt::com_ptr<ID3D12Resource> resource;
        std::size_t capacity = 0;
        D3D12_RESOURCE_FLAGS flags = D3D12_RESOURCE_FLAG_NONE;
        std::uint64_t fenceValue = 0;
        bool stamped = false;
    };

    // Past this a returned buffer is released rather than kept, so an allocation
    // burst is not carried for the rest of the run.
    static constexpr std::size_t reusableBudget = 32 * 1024 * 1024;

    Vector<PooledBuffer> recycling;
    Vector<PooledBuffer> reusable;
    std::size_t reusableBytes = 0;

    // `freeAt` is the fence value that must pass before a slot is lent again;
    // `lent` covers the window before submit stamps the real fence, during which
    // the slot must not reach a second recording.
    struct StagingBuffer
    {
        winrt::com_ptr<ID3D12Resource> resource;
        std::size_t bytes = 0;
        std::uint64_t freeAt = 0;
        bool lent = false;
    };

    Vector<StagingBuffer> staging;

    // The timer only exists while something is pending, so an app that never
    // calls commitAsync never pays for it.
    struct PendingCompletion
    {
        std::uint64_t fenceValue = 0;
        Callback done;
    };

    static constexpr int completionPollHz = 240;

    Vector<PendingCompletion> pendingCompletions;
    std::optional<Threads::Timer> completionPoll;
};

// Created on first use. Main-thread only, like the rest of the GPU backend.
D3D12Context& getD3D12Context();
} // namespace eacp::GPU
