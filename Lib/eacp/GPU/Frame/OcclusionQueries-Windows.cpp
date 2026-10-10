#include <eacp/Core/Utils/WinInclude.h>

#include "OcclusionQueries.h"

#include "../Device/Device.h"
#include "../Windows/D3D12Types.h"

#include <algorithm>
#include <cstring>
#include <utility>

// Windows/D3D12 backend. An OCCLUSION query heap of the set's size, which
// carries both OCCLUSION and BINARY_OCCLUSION queries, and the readback buffer
// each pass resolves the queries it used into - mapped for the set's life, as
// the frame timer's are. Which submission last wrote it is the recording a
// pass named it on, asked by id since a recording is recycled once its fence
// passes.

namespace eacp::GPU
{
struct OcclusionQueries::Native
{
    // Whether the recording that last named the set has finished: recycled
    // under a new id (which only happens once its fence has passed, or once it
    // was discarded and will never run), or submitted and past its fence.
    // `pending` is a recording not yet submitted, which no wait would end.
    enum class Writer
    {
        Done,
        Running,
        Pending
    };

    Writer writerState() const
    {
        if (recording == nullptr || recording->recordingId != recordingId)
            return Writer::Done;

        if (recording->fenceValue == 0)
            return Writer::Pending;

        return getD3D12Context(*device).hasCompleted(recording->fenceValue)
                   ? Writer::Done
                   : Writer::Running;
    }

    winrt::com_ptr<ID3D12QueryHeap> heap;
    winrt::com_ptr<ID3D12Resource> readback;
    const std::uint64_t* results = nullptr;

    CommandContext* recording = nullptr;
    std::uint64_t recordingId = 0;

    Device* device = nullptr;
    int count = 0;
};

OcclusionQueries::OcclusionQueries(Device& device, int count)
{
    impl->device = &device;

    if (count < 1 || !device.supportsOcclusionQueries())
        return;

    auto& context = getD3D12Context(device);
    auto* d3d12 = context.getDevice();

    if (!context.isValid() || d3d12 == nullptr)
        return;

    D3D12_QUERY_HEAP_DESC heapDescriptor = {};
    heapDescriptor.Type = D3D12_QUERY_HEAP_TYPE_OCCLUSION;
    heapDescriptor.Count = static_cast<UINT>(count);

    D3D12_HEAP_PROPERTIES heapProperties = {};
    heapProperties.Type = D3D12_HEAP_TYPE_READBACK;

    const auto bytes = static_cast<UINT64>(count) * sizeof(std::uint64_t);

    D3D12_RESOURCE_DESC bufferDescriptor = {};
    bufferDescriptor.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    bufferDescriptor.Width = bytes;
    bufferDescriptor.Height = 1;
    bufferDescriptor.DepthOrArraySize = 1;
    bufferDescriptor.MipLevels = 1;
    bufferDescriptor.Format = DXGI_FORMAT_UNKNOWN;
    bufferDescriptor.SampleDesc.Count = 1;
    bufferDescriptor.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

    if (FAILED(d3d12->CreateQueryHeap(&heapDescriptor,
                                      IID_PPV_ARGS(impl->heap.put()))))
        return;

    if (FAILED(d3d12->CreateCommittedResource(&heapProperties,
                                              D3D12_HEAP_FLAG_NONE,
                                              &bufferDescriptor,
                                              D3D12_RESOURCE_STATE_COPY_DEST,
                                              nullptr,
                                              IID_PPV_ARGS(impl->readback.put()))))
    {
        impl->heap = nullptr;
        return;
    }

    void* mapped = nullptr;

    if (FAILED(impl->readback->Map(0, nullptr, &mapped)) || mapped == nullptr)
    {
        impl->heap = nullptr;
        impl->readback = nullptr;
        return;
    }

    // Zero until a pass writes them, as on the other backends. Nothing on the
    // GPU has touched the buffer yet, so the CPU write is safe.
    std::memset(mapped, 0, static_cast<std::size_t>(bytes));

    impl->results = static_cast<const std::uint64_t*>(mapped);
    impl->count = count;
}

// A pass that resolved into the buffer, or counted in the heap, may still be
// in flight, so both go the way every buffer and pipeline does: held until
// the GPU has finished what was submitted and no recording is open.
OcclusionQueries::~OcclusionQueries()
{
    if (impl->results == nullptr)
        return;

    auto& context = getD3D12Context(*impl->device);
    context.deferRelease(std::move(impl->heap));
    context.deferRelease(std::move(impl->readback));
}

bool OcclusionQueries::isValid() const
{
    return impl->results != nullptr;
}

int OcclusionQueries::size() const
{
    return impl->count;
}

bool OcclusionQueries::isComplete() const
{
    return impl->writerState() == Native::Writer::Done;
}

void OcclusionQueries::read(std::span<std::uint64_t> results, int first) const
{
    if (impl->results == nullptr || first < 0 || first >= impl->count
        || results.empty())
        return;

    impl->device->assertOwningThread();

    if (impl->writerState() == Native::Writer::Running)
        getD3D12Context(*impl->device).waitFor(impl->recording->fenceValue);

    const auto available = static_cast<std::size_t>(impl->count - first);
    const auto copied = std::min(results.size(), available);

    std::memcpy(results.data(), impl->results + first, copied * sizeof(std::uint64_t));
}

void* OcclusionQueries::nativeResults() const
{
    return impl->readback.get();
}

void* OcclusionQueries::nativeQueries() const
{
    return impl->heap.get();
}

void OcclusionQueries::notePass(void* nativeRecording)
{
    auto* commands = static_cast<CommandContext*>(nativeRecording);

    if (commands == nullptr)
        return;

    impl->recording = commands;
    impl->recordingId = commands->recordingId;
}
} // namespace eacp::GPU
