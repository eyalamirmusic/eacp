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

// The D3D12 plumbing shared by every Windows GPU translation unit, split in
// two along the line that decides what a second GPU::Device can be.
//
// D3D12Shared is one per process: the ID3D12Device and the two root
// signatures. All of it is immutable once created, or free-threaded by
// contract, so sharing it costs nothing and duplicating it would cost a second
// driver device.
//
// D3D12Context is one per GPU::Device: the command queue, the fence, the
// command-list pool, the staging and readback pools, the constant ring and the
// descriptor heaps. Every one of those is mutable and none of it is
// synchronized, which is exactly why it cannot be shared — two Devices
// forwarding to one context are two handles to the same pool, and the second
// one's recording lands in the first one's list. A Device is therefore
// single-threaded and owns its context; see the affinity note on acquire().
//
// The 2D graphics layer keeps its own D3D11 device for Direct2D; the two stacks
// only meet in the compositor, which composes swapchains from either device.
// Not part of GPU.h.

namespace eacp::GPU
{
class Device;
class D3D12Context;

// One recording in flight: an allocator/list pair plus the transient upload
// resources (per-draw constant buffers, staging copies) the recorded commands
// reference. Everything is released together once fenceValue has passed.
struct CommandContext
{
    winrt::com_ptr<ID3D12CommandAllocator> allocator;
    winrt::com_ptr<ID3D12GraphicsCommandList> list;
    Vector<winrt::com_ptr<ID3D12Resource>> transients;
    std::uint64_t fenceValue = 0;

    // The context that lent this recording out, so anything holding one — an
    // encoder, a pass — can reach the queue and the constant ring it belongs
    // to without being told which Device it came from.
    D3D12Context* context = nullptr;

    // Staging-pool slots this recording is copying out of. Unlike transients
    // these are not released when the recording is recycled — they go back to
    // the pool, stamped at submit with the fence that frees them again.
    Vector<int> stagingTaken;

    // Readback-pool slots this recording is copying into, on the same terms.
    Vector<int> readbackTaken;

    // Constant-ring pages this recording is bump-allocating from, oldest
    // first, so the last entry is the one still being filled. Returned to the
    // ring at submit on the same terms as stagingTaken.
    Vector<int> constantsTaken;

    // Identifies the recording for buffer state tracking: a buffer first
    // touched under a new id was implicitly promoted from COMMON, so no
    // barrier is needed (buffers decay back to COMMON after every execute).
    std::uint64_t recordingId = 0;
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

// The process-wide half. Created on first use by getD3D12Shared().
class D3D12Shared
{
public:
    D3D12Shared();

    bool isValid() const { return device != nullptr; }

    ID3D12Device* getDevice() const { return device.get(); }
    ID3D12RootSignature* getRenderRootSignature() const
    {
        return renderRootSignature.get();
    }
    ID3D12RootSignature* getComputeRootSignature() const
    {
        return computeRootSignature.get();
    }

    // Bumped every time the device is rebuilt. A context whose own copy is
    // behind this is holding a queue, heaps and pools that belong to a device
    // that no longer exists, and renews them the next time its owning thread
    // asks it for anything. See D3D12Context::renewIfDeviceRecreated.
    std::uint64_t getGeneration() const { return generation; }

    // Drops the device and both root signatures and builds them again after
    // device removal. Resources created on the old device (app buffers,
    // textures, pipelines) stay dead; their owners rebuild via
    // GPUView::onDeviceRestored, mirroring the D3D11 backend's recovery
    // contract.
    void recreateAfterDeviceLoss();

private:
    void createAll();
    void createDevice();
    void createRootSignatures();

    winrt::com_ptr<ID3D12Device> device;
    winrt::com_ptr<ID3D12RootSignature> renderRootSignature;
    winrt::com_ptr<ID3D12RootSignature> computeRootSignature;

    std::uint64_t generation = 1;
};

D3D12Shared& getD3D12Shared();

class D3D12Context
{
public:
    // The queue type is per-context because the queue is: a Device that only
    // ever runs kernels can take D3D12_COMMAND_LIST_TYPE_COMPUTE and be
    // scheduled alongside the graphics queue instead of strictly behind it.
    // DIRECT is the default because a Device that renders needs it.
    explicit D3D12Context(
        D3D12_COMMAND_LIST_TYPE type = D3D12_COMMAND_LIST_TYPE_DIRECT);
    ~D3D12Context();

    D3D12Context(const D3D12Context&) = delete;
    D3D12Context& operator=(const D3D12Context&) = delete;

    bool isValid() const { return queue != nullptr; }

    // Forwarded from the shared half, so a call site holding a context reaches
    // the device and the signatures the same way it reaches the queue.
    ID3D12Device* getDevice() const { return getD3D12Shared().getDevice(); }
    ID3D12RootSignature* getRenderRootSignature() const
    {
        return getD3D12Shared().getRenderRootSignature();
    }
    ID3D12RootSignature* getComputeRootSignature() const
    {
        return getD3D12Shared().getComputeRootSignature();
    }

    ID3D12CommandQueue* getQueue() const { return queue.get(); }

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
    //
    // Callable only from the thread that constructed the Device this context
    // belongs to — nothing in here is synchronized, and the whole point of a
    // context per Device is that it needs no lock. The assertion is the
    // enforcement: a violation is a loud debug failure rather than a pool
    // handing the same recording to two threads.
    CommandContext* acquire();

    // Closes and executes the list, signals the fence and recycles the
    // context. Returns the fence value that completes when the GPU finishes.
    // Same thread rule as acquire().
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
    // a poll on the event loop, so `done` always runs on the thread that owns
    // this context — which must therefore be a thread running an event loop.
    // A worker Device without one commits synchronously instead.
    //
    // A poll rather than a waiter thread because a fence event would need a
    // thread whose only job is to hand the result straight back here. The cost
    // is a completion latency of up to one poll interval, against a dispatch
    // measured in milliseconds.
    void notifyWhenCompleted(std::uint64_t value, Callback done);

    // Copies bytes into the recording's constant ring and returns their
    // address for a root CBV. The page outlives GPU execution, being held by
    // the recording until its fence passes. Returns 0 on failure.
    D3D12_GPU_VIRTUAL_ADDRESS uploadConstants(CommandContext& commands,
                                              const void* data,
                                              std::size_t bytes);

    // A standalone upload-heap buffer pre-filled with the bytes, for staging
    // resource initial data. The caller parks it on a recording.
    winrt::com_ptr<ID3D12Resource> makeUploadBuffer(const void* data,
                                                    std::size_t bytes);

    // An upload-heap buffer of at least `bytes`, borrowed from a pool and
    // returned once `commands` completes on the GPU. Null on failure.
    //
    // For staging that repeats every frame at a size worth pooling — a video
    // frame is a 33 MB upload at 4K and 133 MB at 8K, a model's input tensor
    // 588 KB every run — where creating and destroying a committed resource
    // costs considerably more than the copy it exists for. The caller must not
    // park the result in transients; the pool owns it.
    ID3D12Resource* acquireStagingBuffer(CommandContext& commands,
                                         std::size_t bytes);

    // The download-side sibling of acquireStagingBuffer, out of a pool of
    // readback-heap buffers and on identical terms. Buffer::read stages every
    // download through one of these.
    ID3D12Resource* acquireReadbackBuffer(CommandContext& commands,
                                          std::size_t bytes);

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

    // Rebuilds the queue, fence, pools and heaps against a device that was
    // recreated after removal. Everything the old device owned is dropped
    // rather than released against it.
    //
    // Called on this context's own thread — by renewIfDeviceRecreated at the
    // next acquire — except on the Device driving recovery, whose swapchains
    // are rebuilt in the same breath and so cannot wait for a later call.
    void renewForNewDevice();

    // Makes this context follow the main thread rather than the one that
    // constructed it. Device::shared() is the only caller: it is created
    // lazily, so a worker that merely touched it first — to compile a kernel,
    // say — would otherwise own the process-wide Device, and the UI's next
    // frame would trip the assertion above.
    void followMainThread() { mainThreadOwned = true; }

private:
    struct DescriptorAllocator
    {
        winrt::com_ptr<ID3D12DescriptorHeap> heap;
        Vector<UINT> freeList;
        UINT next = 0;
        UINT capacity = 0;
        UINT descriptorSize = 0;
    };

    // A page of the constant ring: an upload-heap buffer mapped once for its
    // whole lifetime and bump-allocated from, `used` bytes at a time.
    //
    // An upload heap is CPU-write-combined memory the GPU reads directly, and
    // D3D12 permits a resource to stay mapped indefinitely — so the map, the
    // address lookup and the allocation all happen once per page rather than
    // once per constant block, which is what the whole ring exists for.
    struct ConstantPage
    {
        winrt::com_ptr<ID3D12Resource> resource;
        std::byte* mapped = nullptr;
        D3D12_GPU_VIRTUAL_ADDRESS address = 0;
        std::size_t bytes = 0;
        std::size_t used = 0;
        std::uint64_t freeAt = 0;
        bool lent = false;

        std::size_t remaining() const { return bytes - used; }
    };

    // A slot in one of the CPU-visible buffer pools. `freeAt` is the fence
    // value that must pass before it can be lent out again; `lent` marks the
    // window between the acquire and the submit that stamps the real fence,
    // during which the slot must not be handed to a second recording.
    struct StagingBuffer
    {
        winrt::com_ptr<ID3D12Resource> resource;
        std::size_t bytes = 0;
        std::uint64_t freeAt = 0;
        bool lent = false;
    };

    void createAll();
    void createNullDescriptors();
    void releaseAll();
    DescriptorAllocator makeDescriptorAllocator(D3D12_DESCRIPTOR_HEAP_TYPE type,
                                                UINT capacity);
    DescriptorSlot allocateFrom(DescriptorAllocator& allocator);
    void freeFrom(DescriptorAllocator& allocator, const DescriptorSlot& slot);
    std::uint64_t signal();
    void purgeRetired();
    void pollCompletions();
    void deferReleaseUnknown(winrt::com_ptr<IUnknown> object);

    // Renews everything if the shared device was rebuilt since this context
    // was. Cheap enough to sit at the top of acquire(): one integer compare.
    void renewIfDeviceRecreated();

    // The thread rule acquire() and submit() document. Compiled out of a
    // release build, where the whole check is the assertion.
    void assertOwningThread() const;

    // A buffer on one of the CPU-visible heaps, in the one state that heap
    // type is ever used in. The shared body of makeUploadBuffer and both pools.
    winrt::com_ptr<ID3D12Resource> makeHeapBuffer(D3D12_HEAP_TYPE type,
                                                  std::size_t bytes);

    // The pool logic both acquireStagingBuffer and acquireReadbackBuffer are:
    // reuse a free slot that already fits, else grow one, else add one.
    ID3D12Resource* acquirePooled(Vector<StagingBuffer>& pool,
                                  Vector<int>& taken,
                                  std::size_t bytes,
                                  D3D12_HEAP_TYPE heapType);

    void returnPooled(Vector<StagingBuffer>& pool,
                      Vector<int>& taken,
                      std::uint64_t freeAt);

    // Hands a recording's pooled slots, both directions, back to their pools.
    // `freeAt` is the fence value that must pass before they can be lent out
    // again — 0 for a recording that never reached the GPU, so its slots are
    // free at once.
    void returnStaging(CommandContext& commands, std::uint64_t freeAt);

    // The constant ring's sibling of returnStaging, on the same freeAt terms.
    void returnConstantPages(CommandContext& commands, std::uint64_t freeAt);

    // The page `commands` can fit `bytes` into, taking a fresh one from the
    // ring when the open one is full. Null if none can be had.
    ConstantPage* pageFor(CommandContext& commands, std::size_t bytes);

    D3D12_COMMAND_LIST_TYPE queueType = D3D12_COMMAND_LIST_TYPE_DIRECT;

    // The thread that constructed this context, and therefore the one Device
    // it belongs to may be used from. Stamped once and never changed — unless
    // followMainThread() said to track the main thread instead.
    DWORD owningThreadId = 0;
    bool mainThreadOwned = false;

    winrt::com_ptr<ID3D12CommandQueue> queue;
    winrt::com_ptr<ID3D12Fence> fence;
    HANDLE fenceEvent = nullptr;
    std::uint64_t nextFenceValue = 1;
    std::uint64_t lastSubmittedValue = 0;
    std::uint64_t recordingCounter = 0;

    // Which build of the shared device this context's objects belong to.
    std::uint64_t generation = 0;

    // Per-context rather than shared: a heap's free list is mutable, and the
    // duplication is 1024 + 256 descriptors, which is tens of kilobytes. The
    // compute-with-buffers path never touches them at all — buffers bind as
    // root descriptors by GPU address.
    DescriptorAllocator textureDescriptors;
    DescriptorAllocator samplerDescriptors;

    // Allocated once in createNullDescriptors and deliberately never freed.
    DescriptorSlot nullTexture;
    DescriptorSlot nullTextureUAV;
    DescriptorSlot nullSampler;

    OwnedVector<CommandContext> pool;
    Vector<CommandContext*> available;

    struct Retired
    {
        winrt::com_ptr<IUnknown> object;
        std::uint64_t fenceValue = 0;
    };

    Vector<Retired> retired;

    // Upload-heap buffers kept for reuse, and their readback-heap mirror.
    Vector<StagingBuffer> staging;
    Vector<StagingBuffer> readback;

    // The constant ring. Every dispatch and every draw uploads its uniform
    // block through it, so the count that matters is blocks per recording, not
    // bytes: a page holds 256 of them and a recording rarely needs a second.
    Vector<ConstantPage> constantPages;

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

// The context belonging to a Device — its queue, its pools, its heaps.
//
// A resource does not cross Devices: a buffer, texture or recording belongs to
// the context that made it, and purgeRetired's correctness argument (that the
// context is *fully* idle) holds only per context. That is Metal's contract
// anyway, an MTLBuffer belonging to its MTLDevice.
D3D12Context& getD3D12Context(const Device& device);
} // namespace eacp::GPU
