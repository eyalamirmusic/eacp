#include <eacp/Core/Utils/WinInclude.h>

#include "GpuTimestamps.h"

#include "../Device/Device.h"
#include "../Windows/D3D12Types.h"

// Query layout per slot: two per timed pass from index 0, then the frame's own
// pair at the top, D3D12 having no equivalent of MTLCommandBuffer's
// GPUStartTime. A query heap is not CPU-readable, hence the readback buffer.

namespace eacp::GPU
{
namespace
{
constexpr int frameStartQuery = GpuTimestamps::maxTimedPasses * 2;
constexpr int frameEndQuery = frameStartQuery + 1;
constexpr int queriesPerSlot = frameEndQuery + 1;

constexpr UINT64 queryBytes = sizeof(UINT64);
} // namespace

struct GpuTimestamps::Native
{
    struct Slot
    {
        winrt::com_ptr<ID3D12QueryHeap> heap;
        winrt::com_ptr<ID3D12Resource> readback;

        // Mapped for the object's lifetime; a readback buffer is CPU-visible
        // system memory and costs nothing to keep mapped.
        const UINT64* results = nullptr;

        std::uint64_t fenceValue = 0;
        bool submitted = false;
    };

    // Deferred: Device builds this, and Device::shared() has not returned yet.
    void ensureCreated()
    {
        if (tried)
            return;

        tried = true;

        auto& context = getD3D12Context();

        if (!context.isValid())
            return;

        auto* device = context.getDevice();

        D3D12_QUERY_HEAP_DESC heapDescriptor = {};
        heapDescriptor.Type = D3D12_QUERY_HEAP_TYPE_TIMESTAMP;
        heapDescriptor.Count = static_cast<UINT>(queriesPerSlot);

        D3D12_HEAP_PROPERTIES heapProperties = {};
        heapProperties.Type = D3D12_HEAP_TYPE_READBACK;

        D3D12_RESOURCE_DESC bufferDescriptor = {};
        bufferDescriptor.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        bufferDescriptor.Width = queryBytes * static_cast<UINT64>(queriesPerSlot);
        bufferDescriptor.Height = 1;
        bufferDescriptor.DepthOrArraySize = 1;
        bufferDescriptor.MipLevels = 1;
        bufferDescriptor.Format = DXGI_FORMAT_UNKNOWN;
        bufferDescriptor.SampleDesc.Count = 1;
        bufferDescriptor.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

        for (auto& slot: slots)
        {
            if (FAILED(device->CreateQueryHeap(&heapDescriptor,
                                               IID_PPV_ARGS(slot.heap.put()))))
                return;

            if (FAILED(device->CreateCommittedResource(
                    &heapProperties,
                    D3D12_HEAP_FLAG_NONE,
                    &bufferDescriptor,
                    D3D12_RESOURCE_STATE_COPY_DEST,
                    nullptr,
                    IID_PPV_ARGS(slot.readback.put()))))
                return;

            void* mapped = nullptr;

            if (FAILED(slot.readback->Map(0, nullptr, &mapped)))
                return;

            slot.results = static_cast<const UINT64*>(mapped);
        }

        // Ticks per second, constant for the queue's lifetime.
        if (auto* queue = context.getQueue())
            if (FAILED(queue->GetTimestampFrequency(&frequency)) || frequency == 0)
                return;

        supported = frequency != 0;
    }

    Array<Slot, slotCount> slots;

    UINT64 frequency = 0;

    bool supported = false;
    bool tried = false;
};

GpuTimestamps::GpuTimestamps() = default;
GpuTimestamps::~GpuTimestamps() = default;

bool GpuTimestamps::isSupported() const
{
    return impl->supported;
}

void GpuTimestamps::beginSlot(int slot)
{
    impl->ensureCreated();

    if (!impl->supported)
        return;

    impl->slots[slot].submitted = false;
}

void GpuTimestamps::beginRecording(int slot, void* nativeCommandBuffer)
{
    if (!impl->supported)
        return;

    auto* list = static_cast<ID3D12GraphicsCommandList*>(nativeCommandBuffer);

    if (list == nullptr)
        return;

    list->EndQuery(impl->slots[slot].heap.get(),
                   D3D12_QUERY_TYPE_TIMESTAMP,
                   static_cast<UINT>(frameStartQuery));
}

void* GpuTimestamps::nativeSamples(int slot) const
{
    if (!impl->supported)
        return nullptr;

    return impl->slots[slot].heap.get();
}

// Nothing to report without the queries, unlike Metal: the frame total is two
// of the same timestamps, so a device that cannot take them has no total.
bool GpuTimestamps::endSlot(int slot, int passCount, void* nativeCommandBuffer)
{
    if (!impl->supported)
        return false;

    auto* list = static_cast<ID3D12GraphicsCommandList*>(nativeCommandBuffer);

    if (list == nullptr)
        return false;

    auto& entry = impl->slots[slot];

    list->EndQuery(entry.heap.get(),
                   D3D12_QUERY_TYPE_TIMESTAMP,
                   static_cast<UINT>(frameEndQuery));

    // Only the ranges that were written: a query never ended resolves to
    // undefined data, and the debug layer says so.
    if (passCount > 0)
        list->ResolveQueryData(entry.heap.get(),
                               D3D12_QUERY_TYPE_TIMESTAMP,
                               0,
                               static_cast<UINT>(passCount * 2),
                               entry.readback.get(),
                               0);

    list->ResolveQueryData(entry.heap.get(),
                           D3D12_QUERY_TYPE_TIMESTAMP,
                           static_cast<UINT>(frameStartQuery),
                           2,
                           entry.readback.get(),
                           queryBytes * static_cast<UINT64>(frameStartQuery));

    return true;
}

void GpuTimestamps::noteSubmitted(int slot, std::uint64_t fenceValue)
{
    if (!impl->supported)
        return;

    impl->slots[slot].fenceValue = fenceValue;
    impl->slots[slot].submitted = true;
}

bool GpuTimestamps::isSlotComplete(int slot) const
{
    const auto& entry = impl->slots[slot];

    return entry.submitted && getD3D12Context().hasCompleted(entry.fenceValue);
}

double GpuTimestamps::resolveSlot(int slot, int passCount, double* milliseconds)
{
    const auto& entry = impl->slots[slot];

    if (entry.results == nullptr || impl->frequency == 0)
        return 0.0;

    const auto toMilliseconds = [this](UINT64 start, UINT64 end)
    {
        // An unwritten query reads as zero and a disjoint one can read
        // backwards. Both mean "no number".
        if (end <= start)
            return 0.0;

        return (double) (end - start) * 1000.0 / (double) impl->frequency;
    };

    for (auto pass = 0; pass < passCount; ++pass)
        milliseconds[pass] =
            toMilliseconds(entry.results[pass * 2], entry.results[pass * 2 + 1]);

    const auto frameStart = entry.results[frameStartQuery];
    const auto frameEnd = entry.results[frameEndQuery];

    // Absolute tick counts read after the fence passed, so two zeroes mean the
    // writes never landed - the Parallels virtual GPU answers
    // GetTimestampFrequency, creates the heap and resolves nothing.
    if (frameStart == 0 && frameEnd == 0)
        impl->supported = false;

    return toMilliseconds(frameStart, frameEnd);
}
} // namespace eacp::GPU
