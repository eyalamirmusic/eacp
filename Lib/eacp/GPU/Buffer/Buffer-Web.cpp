#include "Buffer.h"

#include "../Device/Device.h"
#include "../WebGPU/WebGPUTypes.h"

#include <cstring>

namespace eacp::GPU
{
namespace
{
// WebGPU refuses a buffer bound any way it was not created for, and nothing
// above this layer promises a Vertex buffer is never read by a kernel.
constexpr WGPUBufferUsage webBufferUsage =
    WGPUBufferUsage_CopySrc | WGPUBufferUsage_CopyDst | WGPUBufferUsage_Vertex
    | WGPUBufferUsage_Index | WGPUBufferUsage_Storage | WGPUBufferUsage_Indirect;

std::size_t roundUpToWord(std::size_t bytes)
{
    return (bytes + 3) & ~std::size_t {3};
}
} // namespace

struct Buffer::Native
{
    Native(Device& device,
           const void* data,
           std::int64_t byteCount,
           BufferUsage usage,
           BufferStorage storage)
        : owner(&device)
    {
        const auto bytes = (std::size_t) (byteCount > 0 ? byteCount : 0);

        bufferData.size = bytes;

        auto& shared = getWebGPUShared();

        if (!shared.isValid() || bytes == 0)
            return;

        bufferData.allocated = roundUpToWord(bytes);

        auto descriptor = WGPU_BUFFER_DESCRIPTOR_INIT;
        descriptor.usage = webBufferUsage;
        descriptor.size = bufferData.allocated;

        bufferData.buffer = wgpuDeviceCreateBuffer(shared.getDevice(), &descriptor);

        if (bufferData.buffer == nullptr)
            return;

        // A Storage buffer keeps device storage whatever was asked for.
        if (storage == BufferStorage::Streaming && usage != BufferUsage::Storage)
        {
            bufferData.streaming = true;
            bufferData.shadow.resize(static_cast<int>(bufferData.allocated));
        }

        if (data != nullptr)
            write(data, bytes, 0);
    }

    ~Native()
    {
        if (bufferData.buffer != nullptr)
            wgpuBufferRelease(bufferData.buffer);
    }

    // writeBuffer takes whole words, so a ragged tail goes up padded: from the
    // shadow where there is one, else with zeros over the allocation's rounding,
    // or over the next bytes when the write is not at the end.
    void write(const void* data, std::size_t count, std::size_t offset)
    {
        auto queue = getWebGPUShared().getQueue();
        const auto padded = roundUpToWord(count);

        if (bufferData.streaming)
        {
            std::memcpy(bufferData.shadow.data() + offset, data, count);
            wgpuQueueWriteBuffer(queue,
                                 bufferData.buffer,
                                 offset,
                                 bufferData.shadow.data() + offset,
                                 padded);
            return;
        }

        if (padded == count)
        {
            wgpuQueueWriteBuffer(queue, bufferData.buffer, offset, data, count);
            return;
        }

        auto words = Vector<std::byte>(static_cast<int>(padded));
        std::memcpy(words.data(), data, count);

        wgpuQueueWriteBuffer(queue, bufferData.buffer, offset, words.data(), padded);
    }

    void update(const void* data, std::int64_t byteCount, std::int64_t byteOffset)
    {
        if (bufferData.buffer == nullptr || data == nullptr || byteCount <= 0
            || byteOffset < 0 || (std::size_t) byteOffset >= bufferData.size)
            return;

        const auto offset = (std::size_t) byteOffset;

        if (offset % 4 != 0)
        {
            LOG("WebGPU: a Buffer update at byte ",
                offset,
                " is refused, writeBuffer taking whole words only");
            return;
        }

        const auto available = bufferData.size - offset;
        const auto bytes = (std::size_t) byteCount;

        write(data, bytes < available ? bytes : available, offset);
    }

    Device* owner = nullptr;
    mutable WebBufferData bufferData;
};

Buffer::Buffer(Device& device,
               const void* data,
               std::int64_t bytes,
               BufferUsage usage,
               BufferStorage storage)
    : impl(device, data, bytes, usage, storage)
{
    if (isValid())
        device.noteBufferCreated();
}

// Wasm memory is not GPU memory, so the bytes are copied into a buffer of our
// own and released as soon as the copy is taken - see ExternalMemory.
Buffer::Buffer(Device& device, ExternalMemory memory, BufferUsage usage)
    : Buffer(device,
             isPageAligned(memory) ? memory.bytes : nullptr,
             isPageAligned(memory) ? memory.byteCount : 0,
             usage)
{
    memory.onReleased();
}

bool Buffer::canAdoptMemory(const Device&)
{
    return false;
}

// Wasm has no host pages to align to; the 64 KiB of its own memory pages keeps
// the contract meaningful for code written against the other backends.
std::int64_t Buffer::memoryPageSize()
{
    return 65536;
}

std::int64_t Buffer::size() const
{
    return (std::int64_t) impl->bufferData.size;
}

bool Buffer::isValid() const
{
    return impl->bufferData.buffer != nullptr;
}

// Only the Streaming shape can answer: anything else is a mapAsync the main
// thread would have to wait on.
void Buffer::read(void* dst, std::int64_t byteCount, std::int64_t byteOffset) const
{
    if (impl->owner != nullptr)
        impl->owner->assertOwningThread();

    const auto& data = impl->bufferData;

    if (data.buffer == nullptr || dst == nullptr || byteCount <= 0 || byteOffset < 0
        || (std::size_t) byteOffset >= data.size)
        return;

    if (!data.streaming)
    {
        reportWebUnsupported("Buffer::read of a device buffer");
        return;
    }

    const auto offset = (std::size_t) byteOffset;
    const auto available = data.size - offset;
    const auto bytes = (std::size_t) byteCount;

    std::memcpy(
        dst, data.shadow.data() + offset, bytes < available ? bytes : available);
}

// writeBuffer is ordered on the queue behind everything already submitted, so
// the wait update() owes on the other backends is already in the call.
void Buffer::update(const void* data,
                    std::int64_t byteCount,
                    std::int64_t byteOffset)
{
    if (impl->owner != nullptr)
        impl->owner->assertOwningThread();

    impl->update(data, byteCount, byteOffset);
}

void Buffer::updateUnordered(const void* data,
                             std::int64_t byteCount,
                             std::int64_t byteOffset)
{
    if (impl->owner != nullptr)
        impl->owner->assertOwningThread();

    impl->update(data, byteCount, byteOffset);
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
