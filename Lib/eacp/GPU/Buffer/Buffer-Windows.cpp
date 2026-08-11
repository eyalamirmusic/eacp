#include <eacp/Core/Utils/WinInclude.h>

#include "Buffer.h"

#include "../Device/Device.h"
#include "../Windows/D3D12Types.h"

namespace eacp::GPU
{
namespace
{
D3D12_RESOURCE_FLAGS toResourceFlags(BufferUsage usage)
{
    if (usage == BufferUsage::Storage)
        return D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;

    return D3D12_RESOURCE_FLAG_NONE;
}

winrt::com_ptr<ID3D12Resource> makeDefaultBuffer(ID3D12Device* device,
                                                 std::size_t bytes,
                                                 D3D12_RESOURCE_FLAGS flags)
{
    D3D12_HEAP_PROPERTIES heap = {};
    heap.Type = D3D12_HEAP_TYPE_DEFAULT;

    D3D12_RESOURCE_DESC desc = {};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    desc.Width = bytes;
    desc.Height = 1;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    desc.Flags = flags;

    auto buffer = winrt::com_ptr<ID3D12Resource>();
    device->CreateCommittedResource(&heap,
                                    D3D12_HEAP_FLAG_NONE,
                                    &desc,
                                    D3D12_RESOURCE_STATE_COMMON,
                                    nullptr,
                                    __uuidof(ID3D12Resource),
                                    buffer.put_void());
    return buffer;
}
} // namespace

struct Buffer::Native
{
    Native(Device& device, const void* data, std::size_t bytes, BufferUsage usage)
    {
        bufferData.size = bytes;

        auto& context = getD3D12Context();

        if (!context.isValid() || !device.isValid() || bytes == 0)
            return;

        flags = toResourceFlags(usage);
        capacity = bytes;

        // Only recycle when data overwrites it: a fresh resource is zero-filled,
        // a recycled one still holds the previous owner's bytes.
        if (data != nullptr)
            if (auto spare = context.takeDefaultBuffer(bytes, flags))
            {
                bufferData.resource = std::move(spare);
                capacity = bufferData.resource->GetDesc().Width;
            }

        if (bufferData.resource == nullptr)
            bufferData.resource =
                makeDefaultBuffer(context.getDevice(), bytes, flags);

        // Failed upload invalidates the buffer rather than leaving heap garbage.
        if (bufferData.resource != nullptr && data != nullptr
            && !stage(context, data, bytes))
            bufferData.resource = nullptr;
    }

    // Handed to the context, not released: a still-recording list may reference
    // it, and releasing now fails Close() with OBJECT_DELETED_WHILE_STILL_IN_USE,
    // silently dropping every draw on that list.
    ~Native()
    {
        getD3D12Context().recycleDefaultBuffer(
            std::move(bufferData.resource), capacity, flags);
    }

    // Joins the already-open recording when there is one, so the copy lands in
    // order before the draw that wants the bytes, and costs no extra submission.
    bool stage(D3D12Context& context,
               const void* data,
               std::size_t bytes,
               std::size_t destinationOffset = 0)
    {
        if (bufferData.resource == nullptr)
            return false;

        auto* commands = context.getOpenRecording();
        auto ownsRecording = commands == nullptr;

        if (ownsRecording)
            commands = context.acquire();

        if (commands == nullptr)
            return false;

        auto copied = copyInto(context, *commands, data, bytes, destinationOffset);

        if (!ownsRecording)
            return copied;

        if (copied)
            context.submit(commands);
        else
            context.discard(commands);

        return copied;
    }

    bool copyInto(D3D12Context& context,
                  CommandContext& commands,
                  const void* data,
                  std::size_t bytes,
                  std::size_t destinationOffset)
    {
        auto source = context.allocateUpload(commands, bytes);

        if (!source.isValid())
            return false;

        std::memcpy(source.mapped, data, bytes);

        transitionForUse(commands, bufferData, D3D12_RESOURCE_STATE_COPY_DEST);
        commands.list->CopyBufferRegion(bufferData.resource.get(),
                                        destinationOffset,
                                        source.resource,
                                        source.offset,
                                        bytes);

        return true;
    }

    // Mutable because const read() advances the resource state tracking.
    mutable D3D12BufferData bufferData;

    // Real allocated size, which a recycled resource may exceed the request by.
    std::size_t capacity = 0;
    D3D12_RESOURCE_FLAGS flags = D3D12_RESOURCE_FLAG_NONE;
};

Buffer::Buffer(Device& device,
               const void* data,
               std::size_t bytes,
               BufferUsage usage)
    : impl(device, data, bytes, usage)
{
    // Counts GPU allocations, not calls: an invalid Buffer allocated nothing.
    if (isValid())
        device.noteBufferCreated();
}

std::size_t Buffer::size() const
{
    return impl->bufferData.size;
}

bool Buffer::isValid() const
{
    return impl->bufferData.resource != nullptr;
}

void Buffer::read(void* dst, std::size_t bytes, std::size_t offset) const
{
    auto* source = impl->bufferData.resource.get();

    if (source == nullptr || offset >= impl->bufferData.size)
        return;

    auto available = impl->bufferData.size - offset;
    auto count = bytes < available ? bytes : available;

    auto& context = getD3D12Context();
    auto* commands = context.acquire();

    if (commands == nullptr)
        return;

    D3D12_HEAP_PROPERTIES heap = {};
    heap.Type = D3D12_HEAP_TYPE_READBACK;

    D3D12_RESOURCE_DESC desc = {};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    desc.Width = count;
    desc.Height = 1;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

    auto staging = winrt::com_ptr<ID3D12Resource>();

    if (FAILED(context.getDevice()->CreateCommittedResource(
            &heap,
            D3D12_HEAP_FLAG_NONE,
            &desc,
            D3D12_RESOURCE_STATE_COPY_DEST,
            nullptr,
            __uuidof(ID3D12Resource),
            staging.put_void())))
    {
        context.discard(commands);
        return;
    }

    transitionForUse(*commands, impl->bufferData, D3D12_RESOURCE_STATE_COPY_SOURCE);
    commands->list->CopyBufferRegion(staging.get(), 0, source, offset, count);

    // The copy was enqueued on the same queue as the writes, so waiting on
    // this submission's fence also waits for them.
    context.waitFor(context.submit(commands));

    void* mapped = nullptr;
    const D3D12_RANGE readRange = {0, count};

    if (SUCCEEDED(staging->Map(0, &readRange, &mapped)))
    {
        std::memcpy(dst, mapped, count);

        const D3D12_RANGE noWrite = {0, 0};
        staging->Unmap(0, &noWrite);
    }
}

void Buffer::update(const void* data, std::size_t bytes, std::size_t offset)
{
    if (impl->bufferData.resource == nullptr || data == nullptr || bytes == 0
        || offset >= impl->bufferData.size)
        return;

    auto& context = getD3D12Context();

    if (!context.isValid())
        return;

    auto available = impl->bufferData.size - offset;
    auto count = bytes < available ? bytes : available;
    impl->stage(context, data, count, offset);
}

void* Buffer::nativeBuffer() const
{
    return &impl->bufferData;
}

void* Buffer::nativeReadView() const
{
    return &impl->bufferData;
}

void* Buffer::nativeWriteView() const
{
    return &impl->bufferData;
}
} // namespace eacp::GPU
