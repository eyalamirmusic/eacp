#pragma once

#include <eacp/Core/Utils/WinInclude.h>

#include <d3d12.h>
#include <dxgi1_4.h>

#include <winrt/base.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

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
    std::vector<winrt::com_ptr<ID3D12Resource>> transients;
    std::uint64_t fenceValue = 0;

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

    // Copies bytes into a fresh upload-heap buffer parked on the recording
    // (so it outlives GPU execution) and returns its address for a root CBV.
    // Returns 0 on failure.
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

    // Keeps a resource alive until the GPU finished all work submitted so far
    // (plus the recording currently open, which signals next). D3D11's bind
    // ref-counting did this implicitly; buffer and texture destructors route
    // their resources through here instead of releasing in-flight memory.
    void deferRelease(winrt::com_ptr<ID3D12Resource> resource);

    // Tears everything down and rebuilds on a fresh device after device
    // removal. Resources created on the old device (app buffers, textures,
    // pipelines) stay dead; their owners rebuild via GPUView::onDeviceRestored,
    // mirroring the D3D11 backend's recovery contract.
    void recreateAfterDeviceLoss();

private:
    struct DescriptorAllocator
    {
        winrt::com_ptr<ID3D12DescriptorHeap> heap;
        std::vector<UINT> freeList;
        UINT next = 0;
        UINT capacity = 0;
        UINT descriptorSize = 0;
    };

    void createAll();
    void createDevice();
    void createRootSignatures();
    DescriptorAllocator makeDescriptorAllocator(D3D12_DESCRIPTOR_HEAP_TYPE type,
                                                UINT capacity);
    DescriptorSlot allocateFrom(DescriptorAllocator& allocator);
    void freeFrom(DescriptorAllocator& allocator, const DescriptorSlot& slot);
    std::uint64_t signal();
    void purgeRetired();

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

    DescriptorAllocator textureDescriptors;
    DescriptorAllocator samplerDescriptors;

    std::vector<std::unique_ptr<CommandContext>> pool;
    std::vector<CommandContext*> available;

    struct Retired
    {
        winrt::com_ptr<ID3D12Resource> resource;
        std::uint64_t fenceValue = 0;
    };

    std::vector<Retired> retired;
};

// The process-wide context, created on first use. Main-thread only, like the
// rest of the GPU backend.
D3D12Context& getD3D12Context();
} // namespace eacp::GPU
