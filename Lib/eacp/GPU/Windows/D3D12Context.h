#pragma once

#include <eacp/Core/Utils/Containers.h>
#include <eacp/Core/Utils/WinInclude.h>

#include <d3d12.h>
#include <dxgi1_4.h>

#include <winrt/base.h>

#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <thread>

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
    winrt::com_ptr<ID3D12CommandAllocator> allocator;
    winrt::com_ptr<ID3D12GraphicsCommandList> list;
    Vector<winrt::com_ptr<ID3D12Resource>> transients;
    std::uint64_t fenceValue = 0;

    // Identifies the recording for buffer state tracking: a buffer first
    // touched under a new id was implicitly promoted from COMMON, so no
    // barrier is needed (buffers decay back to COMMON after every execute).
    std::uint64_t recordingId = 0;

    // Persistently mapped linear arena the recording's uniform blocks
    // sub-allocate from (see D3D12Context::uploadConstants). The offset
    // rewinds when the context is recycled; the buffer itself lives on.
    winrt::com_ptr<ID3D12Resource> uploadArena;
    std::uint8_t* uploadArenaMapped = nullptr;
    std::size_t uploadArenaCapacity = 0;
    std::size_t uploadArenaOffset = 0;
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
    D3D12_GPU_DESCRIPTOR_HANDLE getNullSamplerDescriptor() const
    {
        return nullSampler.gpu;
    }

    // An open command list ready for recording. Owned by the caller until it
    // is handed back through submit() or discard().
    CommandContext* acquire();

    // Closes and executes the list, signals the fence and recycles the
    // context. Returns the fence value that completes when the GPU finishes.
    std::uint64_t submit(CommandContext* commands);

    // Recycles a recording that should never reach the GPU.
    void discard(CommandContext* commands);

    std::uint64_t lastSubmitted() const { return lastSubmittedValue; }
    bool hasCompleted(std::uint64_t value) const;
    void waitFor(std::uint64_t value);
    void waitIdle();

    // The frame recording currently open, if any (set by Frame for its
    // lifetime). Work that would otherwise submit its own little command
    // list — a texture upload, typically — records into it instead: each
    // ExecuteCommandLists can block for tens of milliseconds while DWM
    // processes a live window resize, so a frame's work must reach the queue
    // as ONE submission, not six.
    CommandContext* activeRecording() const { return activeFrame; }
    void setActiveRecording(CommandContext* commands) { activeFrame = commands; }

    // Like submit(), but the ExecuteCommandLists / fence signal / present run
    // on the context's worker thread: during a live resize the kernel can
    // block every queue submission (and Present) for tens of milliseconds,
    // and the render is driven from the very thread the OS drag loop tracks
    // the mouse with. The fence value is assigned here, in call order, so
    // signals stay monotonic; acquire(), submit(), waitFor() and waitIdle()
    // all drain the in-flight job first. Callers gate on
    // isAsyncSubmitPending() instead of blocking (see GPUView::render).
    std::uint64_t submitAsync(CommandContext* commands,
                              IDXGISwapChain3* presentTarget);
    bool isAsyncSubmitPending();
    void drainAsyncSubmit();

    // Copies bytes into the recording's upload arena and returns their
    // address for a root CBV. Sub-allocation, not allocation: one committed
    // resource per uniform block was the single largest CPU cost of a busy
    // frame, so blocks bump-allocate from a persistently mapped arena that
    // rewinds when the recording is recycled. Returns 0 on failure.
    D3D12_GPU_VIRTUAL_ADDRESS uploadConstants(CommandContext& commands,
                                              const void* data,
                                              std::size_t bytes);

    // A standalone upload-heap buffer pre-filled with the bytes, for staging
    // resource initial data. The caller parks it on a recording.
    winrt::com_ptr<ID3D12Resource> makeUploadBuffer(const void* data,
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
    bool growUploadArena(CommandContext& commands, std::size_t atLeast);
    void createNullDescriptors();
    DescriptorAllocator makeDescriptorAllocator(D3D12_DESCRIPTOR_HEAP_TYPE type,
                                                UINT capacity);
    DescriptorSlot allocateFrom(DescriptorAllocator& allocator);
    void freeFrom(DescriptorAllocator& allocator, const DescriptorSlot& slot);
    std::uint64_t signal();
    void purgeRetired();
    void deferReleaseUnknown(winrt::com_ptr<IUnknown> object);
    void ensureAsyncThread();
    void stopAsyncThread();
    void asyncSubmitLoop();

    winrt::com_ptr<ID3D12Device> device;
    winrt::com_ptr<ID3D12CommandQueue> queue;
    winrt::com_ptr<ID3D12Fence> fence;
    HANDLE fenceEvent = nullptr;
    std::uint64_t nextFenceValue = 1;
    std::uint64_t lastSubmittedValue = 0;
    std::uint64_t recordingCounter = 0;
    std::uint64_t generation = 1;

    CommandContext* activeFrame = nullptr;

    // See submitAsync. The job's CommandContext is recycled on the main
    // thread (by the drain), never by the worker: acquire() and the pools it
    // touches are main-thread structures.
    struct AsyncSubmit
    {
        CommandContext* commands = nullptr;
        IDXGISwapChain3* present = nullptr;
        std::uint64_t fenceValue = 0;
    };

    std::thread asyncThread;
    std::mutex asyncMutex;
    std::condition_variable asyncWake;
    std::condition_variable asyncDone;
    AsyncSubmit asyncJob;
    CommandContext* asyncReap = nullptr;
    bool asyncBusy = false;
    bool asyncQuit = false;

    winrt::com_ptr<ID3D12RootSignature> renderRootSignature;
    winrt::com_ptr<ID3D12RootSignature> computeRootSignature;

    DescriptorAllocator textureDescriptors;
    DescriptorAllocator samplerDescriptors;

    // Allocated once in createNullDescriptors and deliberately never freed.
    DescriptorSlot nullTexture;
    DescriptorSlot nullSampler;

    OwnedVector<CommandContext> pool;
    Vector<CommandContext*> available;

    struct Retired
    {
        winrt::com_ptr<IUnknown> object;
        std::uint64_t fenceValue = 0;
    };

    Vector<Retired> retired;
};

// The process-wide context, created on first use. Main-thread only, like the
// rest of the GPU backend.
D3D12Context& getD3D12Context();
} // namespace eacp::GPU
