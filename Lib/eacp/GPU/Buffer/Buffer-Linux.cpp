#include "Buffer.h"

#include "../Device/Device.h"
#include "../Vulkan/VulkanTypes.h"

#include <cstring>

namespace eacp::GPU
{
namespace
{
// Vulkan refuses a buffer bound any way it was not created for, and nothing
// above this layer promises a Vertex buffer is never read by a kernel.
constexpr VkBufferUsageFlags vulkanBufferUsage =
    VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT
    | VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT
    | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT;
} // namespace

struct Buffer::Native
{
    Native(Device& device,
           const void* data,
           int byteCount,
           BufferUsage usage,
           BufferStorage storage)
        : context(getVulkanContext(device))
    {
        const auto bytes = (std::size_t) (byteCount > 0 ? byteCount : 0);

        bufferData.size = bytes;

        if (!context.isValid() || bytes == 0)
            return;

        // A Storage buffer keeps device storage whatever was asked for.
        if (storage == BufferStorage::Streaming && usage != BufferUsage::Storage)
            if (mapStreamingStorage(data, bytes))
                return;

        if (!makeDeviceBuffer(bytes))
            return;

        // Initial data that never arrived leaves the buffer invalid.
        if (data != nullptr && !stage(data, bytes))
            release();
    }

    // Deferred, a recording still naming the old handle.
    ~Native() { release(); }

    void release()
    {
        context.deferReleaseBuffer(bufferData.buffer, bufferData.allocation);

        bufferData.buffer = VK_NULL_HANDLE;
        bufferData.allocation = nullptr;
        bufferData.mapped = nullptr;
    }

    bool makeDeviceBuffer(std::size_t bytes)
    {
        VkBufferCreateInfo info = {};
        info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        info.size = bytes;
        info.usage = vulkanBufferUsage;
        info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

        // No host-access flag, which is what puts this in device memory.
        VmaAllocationCreateInfo allocationInfo = {};
        allocationInfo.usage = VMA_MEMORY_USAGE_AUTO;

        return vmaCreateBuffer(context.getAllocator(),
                               &info,
                               &allocationInfo,
                               &bufferData.buffer,
                               &bufferData.allocation,
                               nullptr)
               == VK_SUCCESS;
    }

    // False leaves the caller to fall through to the device path.
    bool mapStreamingStorage(const void* data, std::size_t bytes)
    {
        VkBufferCreateInfo info = {};
        info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        info.size = bytes;
        info.usage = vulkanBufferUsage;
        info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

        VmaAllocationCreateInfo allocationInfo = {};
        allocationInfo.usage = VMA_MEMORY_USAGE_AUTO;
        allocationInfo.flags =
            VMA_ALLOCATION_CREATE_MAPPED_BIT
            | VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT;
        allocationInfo.requiredFlags = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT
                                       | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;

        VmaAllocationInfo result = {};

        if (vmaCreateBuffer(context.getAllocator(),
                            &info,
                            &allocationInfo,
                            &bufferData.buffer,
                            &bufferData.allocation,
                            &result)
            != VK_SUCCESS)
            return false;

        bufferData.mapped = static_cast<std::byte*>(result.pMappedData);

        if (bufferData.mapped == nullptr)
        {
            release();
            return false;
        }

        if (data != nullptr)
            std::memcpy(bufferData.mapped, data, bytes);

        return true;
    }

    // Recorded onto whatever recording is open, so the copy lands ahead of the
    // dispatch that wanted the bytes. A render pass gets a recording of its own.
    bool stage(const void* data, std::size_t bytes, std::size_t destination = 0)
    {
        if (bufferData.buffer == VK_NULL_HANDLE)
            return false;

        auto* commands = context.getRecordingForCopy();
        const auto ownsRecording = commands == nullptr;

        if (ownsRecording)
            commands = context.acquire();

        if (commands == nullptr)
            return false;

        const auto copied = copyInto(*commands, data, bytes, destination);

        if (!ownsRecording)
            return copied;

        if (copied)
            context.submit(commands);
        else
            context.discard(commands);

        return copied;
    }

    bool copyInto(CommandContext& commands,
                  const void* data,
                  std::size_t bytes,
                  std::size_t destination)
    {
        auto source = context.allocateUpload(commands, bytes);

        if (!source.isValid())
            return false;

        std::memcpy(source.mapped, data, bytes);

        transitionForUse(commands, bufferData, bufferTransferWrite);

        VkBufferCopy region = {};
        region.srcOffset = source.offset;
        region.dstOffset = static_cast<VkDeviceSize>(destination);
        region.size = static_cast<VkDeviceSize>(bytes);

        vkCmdCopyBuffer(
            commands.buffer, source.buffer, bufferData.buffer, 1, &region);

        return true;
    }

    // A buffer never moves between Devices.
    VulkanContext& context;

    // Mutable because the use tracking advances inside the const read().
    mutable VulkanBufferData bufferData;
};

Buffer::Buffer(Device& device,
               const void* data,
               int bytes,
               BufferUsage usage,
               BufferStorage storage)
    : impl(device, data, bytes, usage, storage)
{
    // Only the ones that got storage, so the count means allocations.
    if (isValid())
        device.noteBufferCreated();
}

int Buffer::size() const
{
    return (int) impl->bufferData.size;
}

bool Buffer::isValid() const
{
    return impl->bufferData.buffer != VK_NULL_HANDLE;
}

void Buffer::read(void* dst, int byteCount, int byteOffset) const
{
    if (impl->bufferData.buffer == VK_NULL_HANDLE || byteCount <= 0 || byteOffset < 0
        || (std::size_t) byteOffset >= impl->bufferData.size)
        return;

    const auto offset = (std::size_t) byteOffset;
    const auto bytes = (std::size_t) byteCount;

    const auto available = impl->bufferData.size - offset;
    const auto count = bytes < available ? bytes : available;

    // Nothing on the GPU writes host storage, so there is nothing to wait for.
    if (impl->bufferData.mapped != nullptr)
    {
        std::memcpy(dst, impl->bufferData.mapped + offset, count);
        return;
    }

    auto& context = impl->context;
    auto* commands = context.acquire();

    if (commands == nullptr)
        return;

    std::byte* mapped = nullptr;
    auto staging = context.acquireReadbackBuffer(*commands, count, mapped);

    if (staging == VK_NULL_HANDLE || mapped == nullptr)
    {
        context.discard(commands);
        return;
    }

    // Use tracking is per recording, and this one lives beside whatever is open:
    // stamping it would cost that one a barrier it may still owe.
    const auto trackedRecording = impl->bufferData.recordingId;
    const auto trackedUse = impl->bufferData.use;

    transitionForUse(*commands, impl->bufferData, bufferTransferRead);

    VkBufferCopy region = {};
    region.srcOffset = static_cast<VkDeviceSize>(offset);
    region.size = static_cast<VkDeviceSize>(count);

    vkCmdCopyBuffer(commands->buffer, impl->bufferData.buffer, staging, 1, &region);

    context.waitFor(context.submit(commands));

    impl->bufferData.recordingId = trackedRecording;
    impl->bufferData.use = trackedUse;

    std::memcpy(dst, mapped, count);
}

void Buffer::update(const void* data, int byteCount, int byteOffset)
{
    if (impl->bufferData.buffer == VK_NULL_HANDLE || data == nullptr
        || byteCount <= 0 || byteOffset < 0
        || (std::size_t) byteOffset >= impl->bufferData.size)
        return;

    if (!impl->context.isValid())
        return;

    const auto offset = (std::size_t) byteOffset;
    const auto bytes = (std::size_t) byteCount;

    const auto available = impl->bufferData.size - offset;
    const auto count = bytes < available ? bytes : available;

    // Unordered against everything already recorded: not writing bytes an
    // in-flight frame reads is the caller's contract under Streaming.
    if (impl->bufferData.mapped != nullptr)
    {
        std::memcpy(impl->bufferData.mapped + offset, data, count);
        return;
    }

    impl->stage(data, count, offset);
}

void* Buffer::nativeBuffer() const
{
    return &impl->bufferData;
}

// One std430 block whichever way the kernel uses it.
void* Buffer::nativeReadView() const
{
    return &impl->bufferData;
}

void* Buffer::nativeWriteView() const
{
    return &impl->bufferData;
}
} // namespace eacp::GPU
