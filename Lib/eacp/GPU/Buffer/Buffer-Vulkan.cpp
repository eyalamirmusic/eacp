#include "Buffer.h"

#include "../Device/Device.h"
#include "../Vulkan/VulkanContext.h"
#include "../Vulkan/VulkanTypes.h"

#include <algorithm>
#include <cstring>

namespace eacp::GPU
{
namespace
{
VkBufferUsageFlags usageFlags(BufferUsage usage)
{
    auto flags = VkBufferUsageFlags {VK_BUFFER_USAGE_TRANSFER_DST_BIT
                                     | VK_BUFFER_USAGE_TRANSFER_SRC_BIT};

    switch (usage)
    {
        case BufferUsage::Vertex:
            return flags | VK_BUFFER_USAGE_VERTEX_BUFFER_BIT;
        case BufferUsage::Index:
            return flags | VK_BUFFER_USAGE_INDEX_BUFFER_BIT;
        case BufferUsage::Storage:
            return flags | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
    }

    return flags;
}
} // namespace

struct Buffer::Native
{
    Native(const void* data, std::size_t bytes, BufferUsage usage)
    {
        auto& context = getVulkanContext();

        if (!context.isValid() || bytes == 0)
            return;

        auto info =
            makeVulkanInfo<VkBufferCreateInfo>(VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO);
        info.size = bytes;
        info.usage = usageFlags(usage);
        info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

        if (vkCreateBuffer(context.getDevice(), &info, nullptr, &buffer.buffer)
            != VK_SUCCESS)
            return;

        auto allocation =
            context.allocateFor(buffer.buffer,
                                VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT
                                    | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);

        if (allocation.memory == VK_NULL_HANDLE)
        {
            vkDestroyBuffer(context.getDevice(), buffer.buffer, nullptr);
            buffer.buffer = VK_NULL_HANDLE;
            return;
        }

        buffer.memory = allocation.memory;
        buffer.bytes = bytes;

        vkMapMemory(context.getDevice(), buffer.memory, 0, bytes, 0, &buffer.mapped);

        if (data != nullptr && buffer.mapped != nullptr)
            std::memcpy(buffer.mapped, data, bytes);
    }

    ~Native()
    {
        auto& context = getVulkanContext();

        if (!context.isValid() || buffer.buffer == VK_NULL_HANDLE)
            return;

        if (buffer.mapped != nullptr)
            vkUnmapMemory(context.getDevice(), buffer.memory);

        context.deferDestroy(buffer.buffer, buffer.memory);
    }

    VulkanBuffer buffer;
};

Buffer::Buffer(Device&, const void* data, std::size_t bytes, BufferUsage usage)
    : impl(data, bytes, usage)
{
}

std::size_t Buffer::size() const
{
    return static_cast<std::size_t>(impl->buffer.bytes);
}

bool Buffer::isValid() const
{
    return impl->buffer.buffer != VK_NULL_HANDLE;
}

void Buffer::read(void* dst, std::size_t bytes) const
{
    if (dst == nullptr || impl->buffer.mapped == nullptr)
        return;

    std::memcpy(dst, impl->buffer.mapped, std::min(bytes, size()));
}

void Buffer::update(const void* data, std::size_t bytes)
{
    if (data == nullptr || impl->buffer.mapped == nullptr)
        return;

    std::memcpy(impl->buffer.mapped, data, std::min(bytes, size()));
}

void* Buffer::nativeBuffer() const
{
    if (!isValid())
        return nullptr;

    return const_cast<VulkanBuffer*>(&impl->buffer);
}

void* Buffer::nativeReadView() const
{
    return nativeBuffer();
}

void* Buffer::nativeWriteView() const
{
    return nativeBuffer();
}
} // namespace eacp::GPU
