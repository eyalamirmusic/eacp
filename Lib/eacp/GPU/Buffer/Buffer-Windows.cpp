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

    // Handed to the context rather than released here, the same way a Texture
    // is. A buffer is routinely replaced mid-frame — every ShaderProgram
    // setInstances/setVertices makes a new one — and the command list still
    // recording holds references to the old resource. Releasing it now makes
    // Close() fail with OBJECT_DELETED_WHILE_STILL_IN_USE, which invalidates
    // the whole list: not just the draw that used the buffer, but every draw
    // recorded after it silently disappears.
    ~Native() { getD3D12Context().deferRelease(std::move(bufferData.resource)); }

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
    // Only the ones that got storage, so the count means GPU allocations rather
    // than calls - a zero-byte or device-less Buffer allocated nothing.
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

    // Out of the pool, for the reason the upload side takes one: a read
    // repeats every run at the same size, and creating a committed resource
    // costs more than the copy. The pool owns it - it must not be parked in
    // transients, and it stays valid until the fence this submission signals.
    auto* staging = context.acquireReadbackBuffer(*commands, count);

    if (staging == nullptr)
    {
        context.discard(commands);
        return;
    }

    transitionForUse(*commands, impl->bufferData, D3D12_RESOURCE_STATE_COPY_SOURCE);
    commands->list->CopyBufferRegion(staging, 0, source, offset, count);

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
    auto* resource = impl->bufferData.resource.get();

    if (resource == nullptr || data == nullptr || bytes == 0
        || offset >= impl->bufferData.size)
        return;

    auto& context = getD3D12Context();

    if (!context.isValid())
        return;

    auto available = impl->bufferData.size - offset;
    auto count = bytes < available ? bytes : available;
    auto* commands = context.acquire();

    if (commands == nullptr)
        return;

    // Out of the pool rather than freshly created: an update repeats every
    // frame at the same size - a model's input tensor, a video frame - and
    // creating a committed resource costs far more than the copy it exists
    // for. The pool owns it, so it must not be parked in transients.
    auto* staging = context.acquireStagingBuffer(*commands, count);

    if (staging == nullptr)
    {
        context.discard(commands);
        return;
    }

    void* mapped = nullptr;
    const D3D12_RANGE noRead = {0, 0};

    if (FAILED(staging->Map(0, &noRead, &mapped)))
    {
        context.discard(commands);
        return;
    }

    std::memcpy(mapped, data, count);
    staging->Unmap(0, nullptr);

    transitionForUse(*commands, impl->bufferData, D3D12_RESOURCE_STATE_COPY_DEST);
    commands->list->CopyBufferRegion(resource, offset, staging, 0, count);
    context.submit(commands);
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
