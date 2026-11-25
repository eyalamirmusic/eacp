#include "Buffer.h"

#include "../Device/Device.h"
#include "../WebGPU/WebGPUContext.h"

#include <eacp/Core/Utils/Logging.h>

#include <cstring>

namespace eacp::GPU
{
namespace
{
// WebGPU refuses a buffer bound any way it was not created for, and nothing
// above this layer promises a Vertex buffer is never read by a kernel.
constexpr auto webgpuBufferUsage =
    wgpu::BufferUsage::CopySrc | wgpu::BufferUsage::CopyDst
    | wgpu::BufferUsage::Vertex | wgpu::BufferUsage::Index
    | wgpu::BufferUsage::Storage | wgpu::BufferUsage::Indirect;

void logReadbackRefusedOnce()
{
    static auto logged = false;

    if (logged)
        return;

    logged = true;
    LOG("WebGPU: reading a buffer back waits for the GPU, which needs "
        "eacp-core-nested-pump (eacp_web_executable(<app> NESTED_PUMP)); the "
        "read is skipped");
}
} // namespace

struct Buffer::Native
{
    Native(Device& device, const void* data, std::int64_t byteCount)
        : context(getWebGPUContext(device))
        , owner(&device)
    {
        bufferData.size = static_cast<std::uint64_t>(byteCount > 0 ? byteCount : 0);

        if (!context.isValid() || bufferData.size == 0)
            return;

        auto descriptor = wgpu::BufferDescriptor {};
        descriptor.size = alignTo(bufferData.size, 4);
        descriptor.usage = webgpuBufferUsage;
        descriptor.mappedAtCreation = true;

        bufferData.buffer = context.getDevice().CreateBuffer(&descriptor);

        if (bufferData.buffer == nullptr)
            return;

        const auto mappedSize = toHostSize(descriptor.size);
        auto* mapped = bufferData.buffer.GetMappedRange(0, mappedSize);

        if (mapped != nullptr)
        {
            std::memset(mapped, 0, mappedSize);

            if (data != nullptr)
                std::memcpy(mapped, data, toHostSize(bufferData.size));
        }

        bufferData.buffer.Unmap();
    }

    // queue.writeBuffer takes whole words, so a range off the word grid is
    // widened to it with the bytes around it read back first where they can
    // be, and left as zeroes where they cannot.
    void write(const void* data, std::int64_t byteCount, std::int64_t byteOffset)
    {
        if (bufferData.buffer == nullptr || data == nullptr || byteCount <= 0
            || byteOffset < 0 || (std::uint64_t) byteOffset >= bufferData.size)
            return;

        const auto offset = static_cast<std::uint64_t>(byteOffset);
        const auto available = bufferData.size - offset;
        const auto count =
            std::min(static_cast<std::uint64_t>(byteCount), available);

        const auto start = offset / 4 * 4;
        const auto end = alignTo(offset + count, 4);

        if (start == offset && end == offset + count)
        {
            context.getQueue().WriteBuffer(
                bufferData.buffer, offset, data, toHostSize(count));
            return;
        }

        auto words = Vector<std::uint8_t>(static_cast<int>(toHostSize(end - start)));
        readInto(words.data(), start, end - start);
        std::memcpy(words.data() + (offset - start), data, toHostSize(count));
        context.getQueue().WriteBuffer(
            bufferData.buffer, start, words.data(), words.getSize());
    }

    // A copy into a mappable staging buffer, submitted behind everything
    // already on the queue, then mapped once the GPU is done with it.
    bool readInto(void* dst, std::uint64_t offset, std::uint64_t count) const
    {
        if (!webgpuCanWait())
        {
            logReadbackRefusedOnce();
            return false;
        }

        const auto start = offset / 4 * 4;
        const auto length = alignTo(offset + count, 4) - start;

        auto descriptor = wgpu::BufferDescriptor {};
        descriptor.size = length;
        descriptor.usage = wgpu::BufferUsage::MapRead | wgpu::BufferUsage::CopyDst;
        auto staging = context.getDevice().CreateBuffer(&descriptor);

        auto encoder = context.getDevice().CreateCommandEncoder();
        encoder.CopyBufferToBuffer(bufferData.buffer, start, staging, 0, length);
        auto commands = encoder.Finish();
        context.getQueue().Submit(1, &commands);

        auto done = false;
        auto mapped = false;

        staging.MapAsync(wgpu::MapMode::Read,
                         0,
                         toHostSize(length),
                         wgpu::CallbackMode::AllowSpontaneous,
                         [&](wgpu::MapAsyncStatus status, wgpu::StringView)
                         {
                             mapped = status == wgpu::MapAsyncStatus::Success;
                             webgpuWake(done);
                         });

        webgpuWaitUntil(done);

        if (!mapped)
            return false;

        const auto* bytes = static_cast<const std::uint8_t*>(
            staging.GetConstMappedRange(0, toHostSize(length)));

        if (bytes != nullptr)
            std::memcpy(dst, bytes + (offset - start), toHostSize(count));

        staging.Unmap();
        staging.Destroy();
        return bytes != nullptr;
    }

    WebGPUContext& context;
    Device* owner = nullptr;

    // Released rather than destroyed with the Buffer: a recording not yet
    // submitted may still name it, and the page frees it once nothing does.
    mutable WebGPUBufferData bufferData;
};

Buffer::Buffer(
    Device& device, const void* data, std::int64_t bytes, BufferUsage, BufferStorage)
    : impl(device, data, bytes)
{
    if (isValid())
        device.noteBufferCreated();
}

// WebGPU cannot adopt memory the page owns, so the bytes are copied into a
// buffer of its own and released at once - see ExternalMemory.
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

std::int64_t Buffer::memoryPageSize()
{
    return 65536;
}

std::int64_t Buffer::size() const
{
    return static_cast<std::int64_t>(impl->bufferData.size);
}

bool Buffer::isValid() const
{
    return impl->bufferData.buffer != nullptr;
}

void Buffer::read(void* dst, std::int64_t byteCount, std::int64_t byteOffset) const
{
    if (impl->owner != nullptr)
        impl->owner->assertOwningThread();

    if (impl->bufferData.buffer == nullptr || dst == nullptr || byteCount <= 0
        || byteOffset < 0 || (std::uint64_t) byteOffset >= impl->bufferData.size)
        return;

    const auto offset = static_cast<std::uint64_t>(byteOffset);
    const auto available = impl->bufferData.size - offset;
    const auto count = std::min(static_cast<std::uint64_t>(byteCount), available);

    impl->readInto(dst, offset, count);
}

// queue.writeBuffer lands ahead of every command buffer submitted after it, so
// the queue is the ordering on both paths.
void Buffer::update(const void* data,
                    std::int64_t byteCount,
                    std::int64_t byteOffset)
{
    if (impl->owner != nullptr)
        impl->owner->assertOwningThread();

    impl->write(data, byteCount, byteOffset);
}

void Buffer::updateUnordered(const void* data,
                             std::int64_t byteCount,
                             std::int64_t byteOffset)
{
    if (impl->owner != nullptr)
        impl->owner->assertOwningThread();

    impl->write(data, byteCount, byteOffset);
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
