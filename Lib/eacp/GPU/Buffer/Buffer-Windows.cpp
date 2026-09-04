#include <eacp/Core/Utils/WinInclude.h>

#include "Buffer.h"

#include "../Device/Device.h"
#include "../Windows/D3D12Types.h"

// Windows/D3D12 backend. A BufferStorage::Device buffer is a default-heap
// resource (a Storage buffer additionally allows unordered access); initial
// data goes through a transient upload buffer submitted at construction, and
// read() copies into a readback buffer and blocks on the fence, preserving the
// contract that a read after commit() sees the kernel's output. State is
// tracked per recording in D3D12BufferData; cross-submit ordering comes from
// the single direct queue.
//
// A BufferStorage::Streaming buffer is the other shape entirely: one
// UPLOAD-heap resource, mapped once and kept mapped, whose GPU address is bound
// as vertex, index or constant data in place. Every write is a memcpy through
// that mapping and puts nothing on a command list - no staging chunk, no
// CopyBufferRegion, no barrier either side of it - which is what a renderer
// streaming hundreds of ranges a frame is here for. Correctness comes from the
// caller instead: see BufferStorage in Buffer.h, and StreamingBuffers, which is
// the caller that has it.

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
    Native(Device& device,
           const void* data,
           std::size_t bytes,
           BufferUsage usage,
           BufferStorage storage)
        : context(getD3D12Context(device))
    {
        bufferData.size = bytes;

        if (!context.isValid() || bytes == 0)
            return;

        flags = toResourceFlags(usage);
        capacity = bytes;

        // A Storage buffer is refused host storage rather than given it and
        // left broken: an upload heap takes no ALLOW_UNORDERED_ACCESS, so a
        // kernel could not write it and CreateCommittedResource would refuse
        // the pair outright. Falling through to the device heap costs the copy
        // a stream was trying to avoid and keeps the buffer a buffer.
        if (storage == BufferStorage::Streaming && usage != BufferUsage::Storage)
            if (mapStreamingStorage(data, bytes))
                return;

        // A spare of the right shape only when this buffer arrives with the data
        // to fill it. Created empty, it has to be the zero-filled resource the
        // runtime hands back rather than one still holding somebody else's
        // numbers -- a compute output target read before it is written would
        // otherwise see them.
        if (data != nullptr)
            if (auto spare = context.takeDefaultBuffer(bytes, flags))
            {
                bufferData.resource = std::move(spare);
                capacity = bufferData.resource->GetDesc().Width;
            }

        if (bufferData.resource == nullptr)
            bufferData.resource =
                makeDefaultBuffer(context.getDevice(), bytes, flags);

        // A buffer whose initial data never reached it is not a buffer, and
        // isValid() is how the caller finds out rather than drawing from
        // whatever the heap happened to hold.
        if (bufferData.resource != nullptr && data != nullptr
            && !stage(context, data, bytes))
            bufferData.resource = nullptr;
    }

    // Handed to the context rather than released here, the same way a Texture
    // is. A buffer is routinely replaced mid-frame — every ShaderProgram
    // setInstances/setVertices makes a new one — and the command list still
    // recording holds references to the old resource. Releasing it now makes
    // Close() fail with OBJECT_DELETED_WHILE_STILL_IN_USE, which invalidates
    // the whole list: not just the draw that used the buffer, but every draw
    // recorded after it silently disappears.
    //
    // Offered up for reuse rather than dropped, on the same terms: it is exactly
    // the buffer the replacement is about to ask for.
    //
    // The streaming half goes to deferRelease rather than to the default-buffer
    // free list, which is a pool of one heap type - and it is deferred for the
    // reason everything here is: draws recorded out of this arena may still be
    // on the queue. The mapping needs no Unmap; releasing the resource takes it
    // with it, the way the context's own upload chunks live and die mapped.
    ~Native()
    {
        if (bufferData.uploadHeap)
        {
            context.deferRelease(std::move(bufferData.resource));
            return;
        }

        context.recycleDefaultBuffer(
            std::move(bufferData.resource), capacity, flags);
    }

    // One upload-heap resource, mapped for good. False when either step fails,
    // which leaves the caller to fall through to the default-heap path rather
    // than hand back a buffer that never got storage.
    bool mapStreamingStorage(const void* data, std::size_t bytes)
    {
        auto resource = context.makeUploadBuffer(nullptr, bytes);

        if (resource == nullptr)
            return false;

        // Mapped with an empty read range, which is both true and the cheap
        // answer: the CPU writes this memory and only the GPU reads it, so the
        // runtime is told there is nothing to make readable on the way in.
        void* address = nullptr;
        const D3D12_RANGE noRead = {0, 0};

        if (FAILED(resource->Map(0, &noRead, &address)) || address == nullptr)
            return false;

        bufferData.resource = std::move(resource);
        bufferData.state = D3D12_RESOURCE_STATE_GENERIC_READ;
        bufferData.uploadHeap = true;
        mapped = static_cast<std::uint8_t*>(address);
        capacity = bytes;

        if (data != nullptr)
            std::memcpy(mapped, data, bytes);

        return true;
    }

    // The copy that fills the buffer from CPU bytes, and the whole of what makes
    // a buffer cheap enough to create per draw.
    //
    // Two things, of the same order of cost. The staging bytes are bump-allocated
    // from the recording's upload arena instead of a committed resource made for
    // this one copy, and the copy goes onto whatever recording is already open
    // instead of one acquired and submitted for it alone. A batching renderer
    // builds a buffer per batch flush, and paying a CreateCommittedResource and a
    // queue submission for each is what made a frame of eacp-ui cost sixty
    // milliseconds of driver time on Windows and nothing measurable on Metal.
    //
    // Recording onto the frame's list rather than ahead of it is also what keeps
    // it correct: the copy lands in order, before the draw or dispatch that
    // wanted the bytes, so two flushes of the same program in one frame each read
    // what they were given.
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

    // The Device's context, held for the buffer's lifetime: read(), update()
    // and the deferred release all belong to the queue this resource was made
    // against, and a buffer never moves between Devices.
    D3D12Context& context;

    // Mutable because the state tracking advances inside the const read():
    // the copy to the readback buffer is a use like any other.
    mutable D3D12BufferData bufferData;

    // What the resource actually is, as against what the caller asked for: a
    // recycled one is at least the requested size and often larger, and it is
    // the real size that decides who it can be handed to next.
    std::size_t capacity = 0;
    D3D12_RESOURCE_FLAGS flags = D3D12_RESOURCE_FLAG_NONE;

    // The persistent mapping of an upload-heap buffer, and the one test that
    // tells the two shapes of this class apart: non-null means a write is a
    // memcpy here and a read comes back out of the same bytes.
    std::uint8_t* mapped = nullptr;
};

Buffer::Buffer(Device& device,
               const void* data,
               std::size_t bytes,
               BufferUsage usage,
               BufferStorage storage)
    : impl(device, data, bytes, usage, storage)
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

    // Host storage reads straight back out of the mapping, as Metal's shared
    // buffers always have. Nothing on the GPU writes those bytes, so there is
    // no work to wait for and no readback copy to make - only the reminder
    // that upload-heap memory is write-combined, which makes this a test and
    // debugging path rather than one for a frame loop.
    if (impl->mapped != nullptr)
    {
        std::memcpy(dst, impl->mapped + offset, count);
        return;
    }

    auto& context = impl->context;
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
    if (impl->bufferData.resource == nullptr || data == nullptr || bytes == 0
        || offset >= impl->bufferData.size)
        return;

    auto& context = impl->context;

    if (!context.isValid())
        return;

    auto available = impl->bufferData.size - offset;
    auto count = bytes < available ? bytes : available;

    // The whole of a streamed write. No recording is touched, so nothing
    // orders it against the draws already on the list and nothing needs to:
    // what makes it safe is that the caller does not write bytes an in-flight
    // frame is still reading, which is the contract BufferStorage::Streaming
    // states and StreamingBuffers keeps.
    if (impl->mapped != nullptr)
    {
        std::memcpy(impl->mapped + offset, data, count);
        return;
    }

    // The same path the initial data takes, rather than a pooled staging
    // resource of its own: an update that lands while a frame is recording goes
    // onto that recording, in order ahead of the work that reads it, instead of
    // acquiring and submitting a list per call.
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
