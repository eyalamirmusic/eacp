#include "Buffer.h"

#include "../Device/Device.h"
#include "../Vulkan/VulkanBackend-Linux.h"
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

struct VulkanBufferBackend final : BufferBackend
{
    VulkanBufferBackend(Device& device,
                        const void* data,
                        std::int64_t byteCount,
                        BufferUsage usage,
                        BufferStorage storage)
        : context(getVulkanContext(device))
        , owner(&device)
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
    ~VulkanBufferBackend() override { release(); }

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

    // The copy both update paths end in, so that each of them asserts the
    // owning thread once rather than once on the way through the other.
    void write(const void* data, std::int64_t byteCount, std::int64_t byteOffset)
    {
        if (bufferData.buffer == VK_NULL_HANDLE || data == nullptr || byteCount <= 0
            || byteOffset < 0 || (std::size_t) byteOffset >= bufferData.size)
            return;

        if (!context.isValid())
            return;

        const auto offset = (std::size_t) byteOffset;
        const auto bytes = (std::size_t) byteCount;

        const auto available = bufferData.size - offset;
        const auto count = bytes < available ? bytes : available;

        // Unordered against everything already recorded: not writing bytes an
        // in-flight frame reads is the caller's contract under Streaming.
        if (bufferData.mapped != nullptr)
        {
            std::memcpy(bufferData.mapped + offset, data, count);
            return;
        }

        stage(data, count, offset);
    }

    void assertOwningThread() const
    {
        if (owner != nullptr)
            owner->assertOwningThread();
    }

    std::int64_t size() const override { return (std::int64_t) bufferData.size; }

    bool isValid() const override { return bufferData.buffer != VK_NULL_HANDLE; }

    void read(void* dst,
              std::int64_t byteCount,
              std::int64_t byteOffset) const override
    {
        assertOwningThread();

        if (bufferData.buffer == VK_NULL_HANDLE || byteCount <= 0 || byteOffset < 0
            || (std::size_t) byteOffset >= bufferData.size)
            return;

        const auto offset = (std::size_t) byteOffset;
        const auto bytes = (std::size_t) byteCount;

        const auto available = bufferData.size - offset;
        const auto count = bytes < available ? bytes : available;

        // Nothing on the GPU writes host storage, so there is nothing to wait
        // for.
        if (bufferData.mapped != nullptr)
        {
            std::memcpy(dst, bufferData.mapped + offset, count);
            return;
        }

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

        // Use tracking is per recording, and this one lives beside whatever is
        // open: stamping it would cost that one a barrier it may still owe.
        const auto trackedRecording = bufferData.recordingId;
        const auto trackedUse = bufferData.use;

        transitionForUse(*commands, bufferData, bufferTransferRead);

        VkBufferCopy region = {};
        region.srcOffset = static_cast<VkDeviceSize>(offset);
        region.size = static_cast<VkDeviceSize>(count);

        vkCmdCopyBuffer(commands->buffer, bufferData.buffer, staging, 1, &region);

        context.waitFor(context.submit(commands));

        bufferData.recordingId = trackedRecording;
        bufferData.use = trackedUse;

        std::memcpy(dst, mapped, count);
    }

    void update(const void* data,
                std::int64_t byteCount,
                std::int64_t byteOffset) override
    {
        assertOwningThread();

        // Only the host-mapped shape needs the wait. A device-storage write
        // below is a vkCmdCopyBuffer recorded into the command stream, and the
        // stream is already the ordering - see the rule on Buffer::update.
        if (bufferData.mapped != nullptr && context.isValid())
            context.waitFor(context.lastSubmitted());

        write(data, byteCount, byteOffset);
    }

    void updateUnordered(const void* data,
                         std::int64_t byteCount,
                         std::int64_t byteOffset) override
    {
        assertOwningThread();

        write(data, byteCount, byteOffset);
    }

    void* nativeBuffer() const override { return &bufferData; }

    // One std430 block whichever way the kernel uses it.
    void* nativeReadView() const override { return &bufferData; }

    void* nativeWriteView() const override { return &bufferData; }

    // A buffer never moves between Devices.
    VulkanContext& context;

    // The Device beside it, for the thread rule alone - see
    // Device::assertOwningThread.
    Device* owner = nullptr;

    // Mutable because the use tracking advances inside the const read().
    mutable VulkanBufferData bufferData;
};
} // namespace

std::unique_ptr<BufferBackend> makeVulkanBuffer(Device& device,
                                                const void* data,
                                                std::int64_t bytes,
                                                BufferUsage usage,
                                                BufferStorage storage)
{
    return std::make_unique<VulkanBufferBackend>(
        device, data, bytes, usage, storage);
}
} // namespace eacp::GPU
