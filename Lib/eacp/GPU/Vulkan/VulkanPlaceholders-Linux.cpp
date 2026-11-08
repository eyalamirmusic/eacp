#include "VulkanPlaceholders.h"

#include <algorithm>
#include <iterator>

namespace eacp::GPU
{
namespace
{
constexpr auto placeholderFormat = VK_FORMAT_R8G8B8A8_UNORM;

// Room for any block or array a generated shader declares; a read past it
// is a read the shader makes of a buffer nobody bound.
constexpr auto placeholderBufferBytes = VkDeviceSize {16 * 1024};

bool makePlaceholderImage(VkDevice device,
                          VmaAllocator allocator,
                          bool cube,
                          VkImageUsageFlags usage,
                          VkImage& image,
                          VmaAllocation& allocation,
                          VkImageView& view)
{
    VkImageCreateInfo info = {};
    info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    info.flags = cube ? VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT : 0u;
    info.imageType = VK_IMAGE_TYPE_2D;
    info.format = placeholderFormat;
    info.extent = {1, 1, 1};
    info.mipLevels = 1;
    info.arrayLayers = cube ? 6u : 1u;
    info.samples = VK_SAMPLE_COUNT_1_BIT;
    info.tiling = VK_IMAGE_TILING_OPTIMAL;
    info.usage = usage | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

    VmaAllocationCreateInfo allocationInfo = {};
    allocationInfo.usage = VMA_MEMORY_USAGE_AUTO;

    if (vmaCreateImage(
            allocator, &info, &allocationInfo, &image, &allocation, nullptr)
        != VK_SUCCESS)
    {
        image = VK_NULL_HANDLE;
        return false;
    }

    VkImageViewCreateInfo viewInfo = {};
    viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    viewInfo.image = image;
    viewInfo.viewType = cube ? VK_IMAGE_VIEW_TYPE_CUBE : VK_IMAGE_VIEW_TYPE_2D;
    viewInfo.format = placeholderFormat;
    viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    viewInfo.subresourceRange.levelCount = 1;
    viewInfo.subresourceRange.layerCount = info.arrayLayers;

    return vkCreateImageView(device, &viewInfo, nullptr, &view) == VK_SUCCESS;
}

bool makePlaceholderBuffer(VmaAllocator allocator,
                           VkDeviceSize size,
                           VkBufferUsageFlags usage,
                           VkBuffer& buffer,
                           VmaAllocation& allocation)
{
    VkBufferCreateInfo info = {};
    info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    info.size = size;
    info.usage = usage | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

    VmaAllocationCreateInfo allocationInfo = {};
    allocationInfo.usage = VMA_MEMORY_USAGE_AUTO;

    if (vmaCreateBuffer(
            allocator, &info, &allocationInfo, &buffer, &allocation, nullptr)
        == VK_SUCCESS)
        return true;

    buffer = VK_NULL_HANDLE;
    return false;
}

VkImageMemoryBarrier placeholderBarrier(VkImage image,
                                        int layers,
                                        VkImageLayout from,
                                        VkImageLayout to,
                                        VkAccessFlags srcAccess,
                                        VkAccessFlags dstAccess)
{
    VkImageMemoryBarrier barrier = {};
    barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    barrier.srcAccessMask = srcAccess;
    barrier.dstAccessMask = dstAccess;
    barrier.oldLayout = from;
    barrier.newLayout = to;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = image;
    barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    barrier.subresourceRange.levelCount = 1;
    barrier.subresourceRange.layerCount = static_cast<std::uint32_t>(layers);
    return barrier;
}
} // namespace

bool VulkanPlaceholders::create(VkDevice device,
                                VmaAllocator allocator,
                                VkQueue queue,
                                std::uint32_t queueFamily,
                                const VkPhysicalDeviceLimits& limits)
{
    constexpr auto sampledUsage = VkImageUsageFlags {VK_IMAGE_USAGE_SAMPLED_BIT};

    storage.size = placeholderBufferBytes;
    uniform.size =
        std::min<VkDeviceSize>(placeholderBufferBytes, limits.maxUniformBufferRange);

    return makePlaceholderImage(device,
                                allocator,
                                false,
                                sampledUsage,
                                sampled2D.image,
                                sampled2D.allocation,
                                sampled2D.view)
           && makePlaceholderImage(device,
                                   allocator,
                                   true,
                                   sampledUsage,
                                   sampledCube.image,
                                   sampledCube.allocation,
                                   sampledCube.view)
           && makePlaceholderImage(device,
                                   allocator,
                                   false,
                                   VK_IMAGE_USAGE_STORAGE_BIT,
                                   storage2D.image,
                                   storage2D.allocation,
                                   storage2D.view)
           && makePlaceholderBuffer(allocator,
                                    storage.size,
                                    VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                                    storage.buffer,
                                    storage.allocation)
           && makePlaceholderBuffer(allocator,
                                    uniform.size,
                                    VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
                                    uniform.buffer,
                                    uniform.allocation)
           && clearAll(device, queue, queueFamily);
}

// The Vulkan 1.0 barrier on purpose: this runs before the synchronization2
// entry points are known to be anything.
bool VulkanPlaceholders::clearAll(VkDevice device,
                                  VkQueue queue,
                                  std::uint32_t queueFamily)
{
    VkCommandPoolCreateInfo poolInfo = {};
    poolInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    poolInfo.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
    poolInfo.queueFamilyIndex = queueFamily;

    auto pool = VkCommandPool {VK_NULL_HANDLE};

    if (vkCreateCommandPool(device, &poolInfo, nullptr, &pool) != VK_SUCCESS)
        return false;

    VkCommandBufferAllocateInfo bufferInfo = {};
    bufferInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    bufferInfo.commandPool = pool;
    bufferInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    bufferInfo.commandBufferCount = 1;

    auto commands = VkCommandBuffer {VK_NULL_HANDLE};

    if (vkAllocateCommandBuffers(device, &bufferInfo, &commands) != VK_SUCCESS)
    {
        vkDestroyCommandPool(device, pool, nullptr);
        return false;
    }

    VkCommandBufferBeginInfo begin = {};
    begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(commands, &begin);

    const VkImageMemoryBarrier toTransfer[] = {
        placeholderBarrier(sampled2D.image,
                           1,
                           VK_IMAGE_LAYOUT_UNDEFINED,
                           VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                           0,
                           VK_ACCESS_TRANSFER_WRITE_BIT),
        placeholderBarrier(sampledCube.image,
                           6,
                           VK_IMAGE_LAYOUT_UNDEFINED,
                           VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                           0,
                           VK_ACCESS_TRANSFER_WRITE_BIT),
        placeholderBarrier(storage2D.image,
                           1,
                           VK_IMAGE_LAYOUT_UNDEFINED,
                           VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                           0,
                           VK_ACCESS_TRANSFER_WRITE_BIT)};

    vkCmdPipelineBarrier(commands,
                         VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                         VK_PIPELINE_STAGE_TRANSFER_BIT,
                         0,
                         0,
                         nullptr,
                         0,
                         nullptr,
                         static_cast<std::uint32_t>(std::size(toTransfer)),
                         toTransfer);

    const auto black = VkClearColorValue {};

    for (const auto* placeholder: {&sampled2D, &sampledCube, &storage2D})
    {
        VkImageSubresourceRange range = {};
        range.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        range.levelCount = 1;
        range.layerCount = placeholder == &sampledCube ? 6u : 1u;

        vkCmdClearColorImage(commands,
                             placeholder->image,
                             VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                             &black,
                             1,
                             &range);
    }

    vkCmdFillBuffer(commands, storage.buffer, 0, VK_WHOLE_SIZE, 0);
    vkCmdFillBuffer(commands, uniform.buffer, 0, VK_WHOLE_SIZE, 0);

    const VkImageMemoryBarrier toUse[] = {
        placeholderBarrier(sampled2D.image,
                           1,
                           VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                           VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                           VK_ACCESS_TRANSFER_WRITE_BIT,
                           VK_ACCESS_SHADER_READ_BIT),
        placeholderBarrier(sampledCube.image,
                           6,
                           VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                           VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                           VK_ACCESS_TRANSFER_WRITE_BIT,
                           VK_ACCESS_SHADER_READ_BIT),
        placeholderBarrier(storage2D.image,
                           1,
                           VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                           VK_IMAGE_LAYOUT_GENERAL,
                           VK_ACCESS_TRANSFER_WRITE_BIT,
                           VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT)};

    VkMemoryBarrier buffersWritten = {};
    buffersWritten.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
    buffersWritten.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    buffersWritten.dstAccessMask = VK_ACCESS_SHADER_READ_BIT
                                   | VK_ACCESS_SHADER_WRITE_BIT
                                   | VK_ACCESS_UNIFORM_READ_BIT;

    vkCmdPipelineBarrier(commands,
                         VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                         0,
                         1,
                         &buffersWritten,
                         0,
                         nullptr,
                         static_cast<std::uint32_t>(std::size(toUse)),
                         toUse);

    auto submitted = vkEndCommandBuffer(commands) == VK_SUCCESS;

    if (submitted)
    {
        VkSubmitInfo submit = {};
        submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        submit.commandBufferCount = 1;
        submit.pCommandBuffers = &commands;

        submitted = vkQueueSubmit(queue, 1, &submit, VK_NULL_HANDLE) == VK_SUCCESS
                    && vkQueueWaitIdle(queue) == VK_SUCCESS;
    }

    vkDestroyCommandPool(device, pool, nullptr);
    return submitted;
}

void VulkanPlaceholders::destroy(VkDevice device, VmaAllocator allocator)
{
    for (auto* placeholder: {&sampled2D, &sampledCube, &storage2D})
    {
        if (placeholder->view != VK_NULL_HANDLE)
            vkDestroyImageView(device, placeholder->view, nullptr);

        if (placeholder->image != VK_NULL_HANDLE)
            vmaDestroyImage(allocator, placeholder->image, placeholder->allocation);

        *placeholder = {};
    }

    for (auto* placeholder: {&storage, &uniform})
    {
        if (placeholder->buffer != VK_NULL_HANDLE)
            vmaDestroyBuffer(
                allocator, placeholder->buffer, placeholder->allocation);

        *placeholder = {};
    }
}

VkDescriptorImageInfo VulkanPlaceholders::image(VkDescriptorType type,
                                                VkImageViewType viewType,
                                                VkSampler sampler) const
{
    if (type == VK_DESCRIPTOR_TYPE_STORAGE_IMAGE)
        return {VK_NULL_HANDLE, storage2D.view, VK_IMAGE_LAYOUT_GENERAL};

    const auto view =
        viewType == VK_IMAGE_VIEW_TYPE_CUBE ? sampledCube.view : sampled2D.view;
    const auto withSampler =
        type == VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER ? sampler : VK_NULL_HANDLE;

    return {withSampler, view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
}

VkDescriptorBufferInfo VulkanPlaceholders::storageBuffer() const
{
    return {storage.buffer, 0, storage.size};
}

VkDescriptorBufferInfo VulkanPlaceholders::uniformBuffer() const
{
    return {uniform.buffer, 0, uniform.size};
}
} // namespace eacp::GPU
