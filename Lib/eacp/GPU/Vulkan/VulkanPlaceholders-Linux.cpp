#include "VulkanPlaceholders.h"
#include "VulkanTypes.h"

#include <eacp/Core/Utils/Logging.h>

#include <algorithm>

namespace eacp::GPU
{
namespace
{
// Room for any block or array a generated shader declares; a read past it
// is a read the shader makes of a buffer nobody bound.
constexpr auto placeholderBufferBytes = VkDeviceSize {16 * 1024};

constexpr auto cubeFaceCount = 6u;

bool isCubeView(VkImageViewType viewType)
{
    return viewType == VK_IMAGE_VIEW_TYPE_CUBE
           || viewType == VK_IMAGE_VIEW_TYPE_CUBE_ARRAY;
}

bool isDepthFormat(VkFormat format)
{
    return format == VK_FORMAT_D16_UNORM;
}

struct ImageTypeOfView
{
    VkImageViewType viewType;
    VkImageType imageType;
};

constexpr ImageTypeOfView imageTypesOfViews[] = {
    {VK_IMAGE_VIEW_TYPE_1D, VK_IMAGE_TYPE_1D},
    {VK_IMAGE_VIEW_TYPE_1D_ARRAY, VK_IMAGE_TYPE_1D},
    {VK_IMAGE_VIEW_TYPE_3D, VK_IMAGE_TYPE_3D},
};

VkImageType getImageTypeForView(VkImageViewType viewType)
{
    for (const auto& entry: imageTypesOfViews)
    {
        if (entry.viewType == viewType)
            return entry.imageType;
    }

    return VK_IMAGE_TYPE_2D;
}

std::uint32_t getLayerCount(const VulkanImageShape& shape)
{
    if (isCubeView(shape.viewType))
        return cubeFaceCount;

    return 1u;
}

VkImageAspectFlags getAspect(const VulkanImageShape& shape)
{
    if (isDepthFormat(shape.format))
        return VK_IMAGE_ASPECT_DEPTH_BIT;

    return VK_IMAGE_ASPECT_COLOR_BIT;
}

VkImageUsageFlags getUsage(bool isStorage)
{
    if (isStorage)
        return VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;

    return VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
}

VkImageCreateFlags getCreateFlags(const VulkanImageShape& shape)
{
    if (isCubeView(shape.viewType))
        return VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT;

    return 0;
}

VkImageCreateInfo describeImage(const VulkanImageShape& shape, bool isStorage)
{
    VkImageCreateInfo info = {};
    info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    info.flags = getCreateFlags(shape);
    info.imageType = getImageTypeForView(shape.viewType);
    info.format = shape.format;
    info.extent = {1, 1, 1};
    info.mipLevels = 1;
    info.arrayLayers = getLayerCount(shape);
    info.samples = shape.samples;
    info.tiling = VK_IMAGE_TILING_OPTIMAL;
    info.usage = getUsage(isStorage);
    info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    return info;
}

VkImageSubresourceRange getWholeRange(const VulkanImageShape& shape)
{
    VkImageSubresourceRange range = {};
    range.aspectMask = getAspect(shape);
    range.levelCount = 1;
    range.layerCount = getLayerCount(shape);
    return range;
}

struct MadeBuffer
{
    VkBuffer buffer = VK_NULL_HANDLE;
    VmaAllocation allocation = nullptr;
};

MadeBuffer makePlaceholderBuffer(VmaAllocator allocator,
                                 VkDeviceSize size,
                                 VkBufferUsageFlags usage)
{
    VkBufferCreateInfo info = {};
    info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    info.size = size;
    info.usage = usage | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

    VmaAllocationCreateInfo allocationInfo = {};
    allocationInfo.usage = VMA_MEMORY_USAGE_AUTO;

    auto made = MadeBuffer {};

    if (vmaCreateBuffer(allocator,
                        &info,
                        &allocationInfo,
                        &made.buffer,
                        &made.allocation,
                        nullptr)
        != VK_SUCCESS)
    {
        return {};
    }

    return made;
}

struct LayoutChange
{
    VkImageLayout from;
    VkImageLayout to;
    VkAccessFlags srcAccess;
    VkAccessFlags dstAccess;
};

VkImageMemoryBarrier describeLayoutChange(VkImage image,
                                          const VkImageSubresourceRange& range,
                                          const LayoutChange& change)
{
    VkImageMemoryBarrier barrier = {};
    barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    barrier.srcAccessMask = change.srcAccess;
    barrier.dstAccessMask = change.dstAccess;
    barrier.oldLayout = change.from;
    barrier.newLayout = change.to;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = image;
    barrier.subresourceRange = range;
    return barrier;
}

void recordImageBarrier(VkCommandBuffer commands,
                        VkPipelineStageFlags from,
                        VkPipelineStageFlags to,
                        const VkImageMemoryBarrier& barrier)
{
    vkCmdPipelineBarrier(commands, from, to, 0, 0, nullptr, 0, nullptr, 1, &barrier);
}

void recordClear(VkCommandBuffer commands,
                 VkImage image,
                 const VulkanImageShape& shape,
                 const VkImageSubresourceRange& range)
{
    if (isDepthFormat(shape.format))
    {
        const auto zero = VkClearDepthStencilValue {};
        vkCmdClearDepthStencilImage(
            commands, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &zero, 1, &range);
        return;
    }

    const auto zero = VkClearColorValue {};
    vkCmdClearColorImage(
        commands, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &zero, 1, &range);
}

VkSampler chooseSampler(VkDescriptorType type,
                        const VulkanImageShape& shape,
                        VkSampler sampler,
                        VkSampler compareSampler)
{
    if (type == VK_DESCRIPTOR_TYPE_SAMPLER)
        return sampler;

    if (type != VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER)
        return VK_NULL_HANDLE;

    if (isDepthFormat(shape.format))
        return compareSampler;

    return sampler;
}

VkImageLayout getRestingLayout(bool isStorage)
{
    if (isStorage)
        return VK_IMAGE_LAYOUT_GENERAL;

    return VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
}

void logUnmakeableShape(const VulkanImageShape& shape)
{
    LOG("Vulkan: no placeholder for an unbound ",
        getVulkanViewTypeName(shape.viewType),
        " image of ",
        getVulkanFormatName(shape.format),
        " at ",
        getVulkanSampleCountName(shape.samples),
        "; a draw leaves that slot unwritten when nothing is bound there");
}
} // namespace

bool VulkanPlaceholders::create(const Target& where,
                                const VkPhysicalDeviceLimits& limits,
                                const VkPhysicalDeviceFeatures& enabledFeatures)
{
    target = where;
    canMakeCubeArrays = enabledFeatures.imageCubeArray == VK_TRUE;

    storage.size = placeholderBufferBytes;
    uniform.size =
        std::min<VkDeviceSize>(placeholderBufferBytes, limits.maxUniformBufferRange);

    const auto storageMade = makePlaceholderBuffer(
        target.allocator, storage.size, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
    const auto uniformMade = makePlaceholderBuffer(
        target.allocator, uniform.size, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT);

    storage.buffer = storageMade.buffer;
    storage.allocation = storageMade.allocation;
    uniform.buffer = uniformMade.buffer;
    uniform.allocation = uniformMade.allocation;

    if (storage.buffer == VK_NULL_HANDLE || uniform.buffer == VK_NULL_HANDLE)
        return false;

    return clearBuffers();
}

void VulkanPlaceholders::fill(VulkanTextureBindings& bindings,
                              VkSampler sampler,
                              VkSampler compareSampler)
{
    auto guard = std::lock_guard {imagesMutex};

    for (auto slot = 0; slot < maxTextureSlots; ++slot)
    {
        if (!bindings.has(slot))
            continue;

        const auto type = bindings.typeAt(slot);
        const auto& shape = bindings.shapeAt(slot);
        auto& placeholder = bindings.placeholders[slot];

        placeholder = {};
        placeholder.sampler = chooseSampler(type, shape, sampler, compareSampler);

        if (type == VK_DESCRIPTOR_TYPE_SAMPLER)
            continue;

        const auto isStorage = type == VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;

        if (const auto image = findOrMakeImage(shape, isStorage))
        {
            placeholder.imageView = image->view;
            placeholder.imageLayout = image->layout;
        }
    }
}

std::optional<VulkanPlaceholders::Image>
    VulkanPlaceholders::findOrMakeImage(const VulkanImageShape& shape,
                                        bool isStorage)
{
    const auto isSameShape = [&](const Image& image)
    { return image.shape == shape && image.isStorage == isStorage; };

    const auto* found = images.findIf(isSameShape);

    // A failed shape is kept too, so its log line is said once.
    if (found == nullptr)
    {
        images.add(makeImage(shape, isStorage));
        found = &images.back();
    }

    if (found->view == VK_NULL_HANDLE)
        return std::nullopt;

    return *found;
}

VulkanPlaceholders::Image
    VulkanPlaceholders::makeImage(const VulkanImageShape& shape, bool isStorage)
{
    auto made = Image {};
    made.shape = shape;
    made.isStorage = isStorage;

    if (canMake(shape, isStorage) && createImageAndView(made) && clearImage(made))
        return made;

    logUnmakeableShape(shape);
    destroyImage(made);
    made.shape = shape;
    made.isStorage = isStorage;
    return made;
}

bool VulkanPlaceholders::canMake(const VulkanImageShape& shape, bool isStorage) const
{
    if (shape.viewType == VK_IMAGE_VIEW_TYPE_CUBE_ARRAY && !canMakeCubeArrays)
        return false;

    // shaderStorageImageMultisample is never enabled.
    if (isStorage && shape.samples != VK_SAMPLE_COUNT_1_BIT)
        return false;

    const auto info = describeImage(shape, isStorage);
    VkImageFormatProperties properties = {};

    if (vkGetPhysicalDeviceImageFormatProperties(target.physicalDevice,
                                                 info.format,
                                                 info.imageType,
                                                 info.tiling,
                                                 info.usage,
                                                 info.flags,
                                                 &properties)
        != VK_SUCCESS)
    {
        return false;
    }

    return (properties.sampleCounts & shape.samples) != 0;
}

bool VulkanPlaceholders::createImageAndView(Image& made)
{
    const auto info = describeImage(made.shape, made.isStorage);

    VmaAllocationCreateInfo allocationInfo = {};
    allocationInfo.usage = VMA_MEMORY_USAGE_AUTO;

    if (vmaCreateImage(target.allocator,
                       &info,
                       &allocationInfo,
                       &made.image,
                       &made.allocation,
                       nullptr)
        != VK_SUCCESS)
    {
        made.image = VK_NULL_HANDLE;
        return false;
    }

    VkImageViewCreateInfo viewInfo = {};
    viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    viewInfo.image = made.image;
    viewInfo.viewType = made.shape.viewType;
    viewInfo.format = made.shape.format;
    viewInfo.subresourceRange = getWholeRange(made.shape);

    if (vkCreateImageView(target.device, &viewInfo, nullptr, &made.view)
        != VK_SUCCESS)
    {
        made.view = VK_NULL_HANDLE;
        return false;
    }

    made.layout = getRestingLayout(made.isStorage);
    return true;
}

// The Vulkan 1.0 barrier and submit: every device has them, with or without
// synchronization2.
bool VulkanPlaceholders::clearImage(const Image& made)
{
    const auto range = getWholeRange(made.shape);
    const auto toTransfer = LayoutChange {VK_IMAGE_LAYOUT_UNDEFINED,
                                          VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                          0,
                                          VK_ACCESS_TRANSFER_WRITE_BIT};
    const auto toUse =
        LayoutChange {VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                      made.layout,
                      VK_ACCESS_TRANSFER_WRITE_BIT,
                      VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT};

    return submitAndWait(
        [&](VkCommandBuffer commands)
        {
            recordImageBarrier(commands,
                               VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                               VK_PIPELINE_STAGE_TRANSFER_BIT,
                               describeLayoutChange(made.image, range, toTransfer));

            recordClear(commands, made.image, made.shape, range);

            recordImageBarrier(commands,
                               VK_PIPELINE_STAGE_TRANSFER_BIT,
                               VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                               describeLayoutChange(made.image, range, toUse));
        });
}

bool VulkanPlaceholders::clearBuffers()
{
    return submitAndWait(
        [&](VkCommandBuffer commands)
        {
            vkCmdFillBuffer(commands, storage.buffer, 0, VK_WHOLE_SIZE, 0);
            vkCmdFillBuffer(commands, uniform.buffer, 0, VK_WHOLE_SIZE, 0);

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
                                 0,
                                 nullptr);
        });
}

template <typename Record>
bool VulkanPlaceholders::submitAndWait(const Record& record)
{
    VkCommandPoolCreateInfo poolInfo = {};
    poolInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    poolInfo.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
    poolInfo.queueFamilyIndex = target.queueFamily;

    auto pool = VkCommandPool {VK_NULL_HANDLE};

    if (vkCreateCommandPool(target.device, &poolInfo, nullptr, &pool) != VK_SUCCESS)
        return false;

    VkCommandBufferAllocateInfo bufferInfo = {};
    bufferInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    bufferInfo.commandPool = pool;
    bufferInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    bufferInfo.commandBufferCount = 1;

    auto commands = VkCommandBuffer {VK_NULL_HANDLE};

    if (vkAllocateCommandBuffers(target.device, &bufferInfo, &commands)
        != VK_SUCCESS)
    {
        vkDestroyCommandPool(target.device, pool, nullptr);
        return false;
    }

    VkCommandBufferBeginInfo begin = {};
    begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(commands, &begin);

    record(commands);

    auto submitted = vkEndCommandBuffer(commands) == VK_SUCCESS;

    if (submitted)
    {
        VkSubmitInfo submit = {};
        submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        submit.commandBufferCount = 1;
        submit.pCommandBuffers = &commands;

        auto guard = std::lock_guard {queueMutex};
        submitted =
            vkQueueSubmit(target.queue, 1, &submit, VK_NULL_HANDLE) == VK_SUCCESS
            && vkQueueWaitIdle(target.queue) == VK_SUCCESS;
    }

    vkDestroyCommandPool(target.device, pool, nullptr);
    return submitted;
}

void VulkanPlaceholders::destroyImage(Image& image) const
{
    if (image.view != VK_NULL_HANDLE)
        vkDestroyImageView(target.device, image.view, nullptr);

    if (image.image != VK_NULL_HANDLE)
        vmaDestroyImage(target.allocator, image.image, image.allocation);

    image = {};
}

void VulkanPlaceholders::destroyBuffer(Buffer& buffer) const
{
    if (buffer.buffer != VK_NULL_HANDLE)
        vmaDestroyBuffer(target.allocator, buffer.buffer, buffer.allocation);

    buffer = {};
}

void VulkanPlaceholders::destroy()
{
    for (auto& image: images)
        destroyImage(image);

    images.clear();
    destroyBuffer(storage);
    destroyBuffer(uniform);
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
