#include <eacp/Core/Utils/WinInclude.h>

#include "Buffer.h"

#include "../Device/Device.h"
#include "../Windows/D3D12Types.h"

// Windows/D3D12 backend. Every buffer is a default-heap resource (a Storage
// buffer additionally allows unordered access); initial data goes through a
// transient upload buffer submitted at construction, and read() copies into a
// readback buffer and blocks on the fence, preserving the contract that a read
// after commit() sees the kernel's output. State is tracked per recording in
// D3D12BufferData; cross-submit ordering comes from the single direct queue.

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

        bufferData.resource =
            makeDefaultBuffer(context.getDevice(), bytes, toResourceFlags(usage));

        if (bufferData.resource != nullptr && data != nullptr)
            upload(context, data, bytes);
    }

    void upload(D3D12Context& context, const void* data, std::size_t bytes)
    {
        auto staging = context.makeUploadBuffer(data, bytes);
        auto* commands = context.acquire();

        if (staging == nullptr || commands == nullptr)
        {
            context.discard(commands);
            bufferData.resource = nullptr;
            return;
        }

        commands->list->CopyBufferRegion(
            bufferData.resource.get(), 0, staging.get(), 0, bytes);
        commands->transients.add(std::move(staging));
        context.submit(commands);
    }

    // Mutable because the state tracking advances inside the const read():
    // the copy to the readback buffer is a use like any other.
    mutable D3D12BufferData bufferData;
};

Buffer::Buffer(Device& device,
               const void* data,
               std::size_t bytes,
               BufferUsage usage)
    : impl(device, data, bytes, usage)
{
}

std::size_t Buffer::size() const
{
    return impl->bufferData.size;
}

bool Buffer::isValid() const
{
    return impl->bufferData.resource != nullptr;
}

void Buffer::read(void* dst, std::size_t bytes) const
{
    auto* source = impl->bufferData.resource.get();

    if (source == nullptr)
        return;

    auto& context = getD3D12Context();
    auto* commands = context.acquire();

    if (commands == nullptr)
        return;

    D3D12_HEAP_PROPERTIES heap = {};
    heap.Type = D3D12_HEAP_TYPE_READBACK;

    D3D12_RESOURCE_DESC desc = {};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    desc.Width = impl->bufferData.size;
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
    commands->list->CopyBufferRegion(
        staging.get(), 0, source, 0, impl->bufferData.size);

    // The copy was enqueued on the same queue as the writes, so waiting on
    // this submission's fence also waits for them.
    context.waitFor(context.submit(commands));

    void* mapped = nullptr;
    const D3D12_RANGE readRange = {0, bytes};

    if (SUCCEEDED(staging->Map(0, &readRange, &mapped)))
    {
        std::memcpy(dst, mapped, bytes);

        const D3D12_RANGE noWrite = {0, 0};
        staging->Unmap(0, &noWrite);
    }
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
