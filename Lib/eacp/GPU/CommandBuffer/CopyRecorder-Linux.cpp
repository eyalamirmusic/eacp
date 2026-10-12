#include "CopyRecorder.h"

#include "../Texture/MipChain.h"
#include "../Vulkan/VulkanTypes.h"

#include <eacp/Core/Utils/Logging.h>

#include <algorithm>

namespace eacp::GPU
{
namespace
{
// A copy inside a render pass instance is illegal, and a pass that is still
// open when one arrives has broken the one-encoder rule.
CommandContext* recordingOutsidePasses(void* recording)
{
    auto* commands = static_cast<CommandContext*>(recording);

    if (commands == nullptr || commands->buffer == VK_NULL_HANDLE)
        return nullptr;

    if (commands->context != nullptr && commands->context->isRenderPassOpen())
    {
        LOG("Vulkan: a copy while a render pass is open is dropped; end the pass "
            "first");
        return nullptr;
    }

    return commands;
}

VulkanTextureData* textureDataOf(const Texture& texture)
{
    auto* data = static_cast<VulkanTextureData*>(texture.nativeTexture());
    return data != nullptr && data->image != VK_NULL_HANDLE ? data : nullptr;
}

VulkanBufferData* bufferDataOf(const Buffer& buffer)
{
    auto* data = static_cast<VulkanBufferData*>(buffer.nativeBuffer());
    return data != nullptr && data->buffer != VK_NULL_HANDLE ? data : nullptr;
}

VkImageSubresourceLayers colorLayer(int level, int slice, int count = 1)
{
    VkImageSubresourceLayers layers = {};
    layers.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    layers.mipLevel = static_cast<std::uint32_t>(level);
    layers.baseArrayLayer = static_cast<std::uint32_t>(slice);
    layers.layerCount = static_cast<std::uint32_t>(count);
    return layers;
}

VkBufferImageCopy bufferImageRegion(const BufferTextureCopyPlan& plan)
{
    VkBufferImageCopy region = {};
    region.bufferOffset = static_cast<VkDeviceSize>(plan.bufferOffset);
    region.bufferRowLength =
        static_cast<std::uint32_t>(plan.bytesPerRow / plan.bytesPerTexel);
    region.imageSubresource = colorLayer(plan.level, plan.slice);
    region.imageOffset = {plan.x, plan.y, 0};
    region.imageExtent = {static_cast<std::uint32_t>(plan.width),
                          static_cast<std::uint32_t>(plan.height),
                          1};
    return region;
}

void restAfterCopy(VkCommandBuffer buffer, VulkanTextureData& data)
{
    transitionTextureForUse(buffer, data, data.restingUse());
}
} // namespace

void recordCopy(void* recording, const TextureCopyPlan& plan)
{
    auto* commands = recordingOutsidePasses(recording);
    auto* source = textureDataOf(*plan.source);
    auto* destination = textureDataOf(*plan.destination);

    if (commands == nullptr || source == nullptr || destination == nullptr)
        return;

    auto buffer = commands->buffer;

    transitionTextureForUse(buffer, *source, imageTransferSrc);
    transitionTextureForUse(buffer, *destination, imageTransferDst);

    // A run of layers is one region of that many; a volume's images are the
    // extent's depth.
    VkImageCopy region = {};
    region.srcSubresource =
        colorLayer(plan.sourceLevel, plan.sourceSlice, plan.sliceCount);
    region.srcOffset = {plan.sourceX, plan.sourceY, plan.sourceZ};
    region.dstSubresource =
        colorLayer(plan.destinationLevel, plan.destinationSlice, plan.sliceCount);
    region.dstOffset = {plan.destinationX, plan.destinationY, plan.destinationZ};
    region.extent = {static_cast<std::uint32_t>(plan.width),
                     static_cast<std::uint32_t>(plan.height),
                     static_cast<std::uint32_t>(plan.depth)};

    vkCmdCopyImage(buffer,
                   source->image,
                   imageTransferSrc.layout,
                   destination->image,
                   imageTransferDst.layout,
                   1,
                   &region);

    restAfterCopy(buffer, *source);
    restAfterCopy(buffer, *destination);
}

void recordCopy(void* recording, const BufferCopyPlan& plan)
{
    auto* commands = recordingOutsidePasses(recording);
    auto* source = bufferDataOf(*plan.source);
    auto* destination = bufferDataOf(*plan.destination);

    if (commands == nullptr || source == nullptr || destination == nullptr)
        return;

    transitionForUse(*commands, *source, bufferTransferRead);
    transitionForUse(*commands, *destination, bufferTransferWrite);

    VkBufferCopy region = {};
    region.srcOffset = static_cast<VkDeviceSize>(plan.sourceOffset);
    region.dstOffset = static_cast<VkDeviceSize>(plan.destinationOffset);
    region.size = static_cast<VkDeviceSize>(plan.bytes);

    vkCmdCopyBuffer(
        commands->buffer, source->buffer, destination->buffer, 1, &region);
}

void recordCopyToTexture(void* recording, const BufferTextureCopyPlan& plan)
{
    auto* commands = recordingOutsidePasses(recording);
    auto* buffer = bufferDataOf(*plan.buffer);
    auto* texture = textureDataOf(*plan.texture);

    if (commands == nullptr || buffer == nullptr || texture == nullptr)
        return;

    transitionForUse(*commands, *buffer, bufferTransferRead);
    transitionTextureForUse(commands->buffer, *texture, imageTransferDst);

    const auto region = bufferImageRegion(plan);

    vkCmdCopyBufferToImage(commands->buffer,
                           buffer->buffer,
                           texture->image,
                           imageTransferDst.layout,
                           1,
                           &region);

    restAfterCopy(commands->buffer, *texture);
}

void recordCopyToBuffer(void* recording, const BufferTextureCopyPlan& plan)
{
    auto* commands = recordingOutsidePasses(recording);
    auto* buffer = bufferDataOf(*plan.buffer);
    auto* texture = textureDataOf(*plan.texture);

    if (commands == nullptr || buffer == nullptr || texture == nullptr)
        return;

    transitionForUse(*commands, *buffer, bufferTransferWrite);
    transitionTextureForUse(commands->buffer, *texture, imageTransferSrc);

    const auto region = bufferImageRegion(plan);

    vkCmdCopyImageToBuffer(commands->buffer,
                           texture->image,
                           imageTransferSrc.layout,
                           buffer->buffer,
                           1,
                           &region);

    restAfterCopy(commands->buffer, *texture);
}

namespace
{
// TRANSFER_DST at every transfer stage, the blit's among them: imageTransferDst
// names the copy stage alone, which does not order a blit.
constexpr auto blitDestination = ImageUse {VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                           VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT,
                                           VK_ACCESS_2_TRANSFER_WRITE_BIT};

// One level of every layer from TRANSFER_DST to TRANSFER_SRC, once the blit
// into it has landed, so the next blit reads it.
void makeLevelReadable(VkCommandBuffer buffer, VkImage image, int level, int layers)
{
    VkImageMemoryBarrier2 barrier = {};
    barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
    barrier.srcStageMask = VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT;
    barrier.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
    barrier.dstStageMask = VK_PIPELINE_STAGE_2_BLIT_BIT;
    barrier.dstAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT;
    barrier.oldLayout = imageTransferDst.layout;
    barrier.newLayout = imageTransferSrc.layout;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = image;
    barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    barrier.subresourceRange.baseMipLevel = static_cast<std::uint32_t>(level);
    barrier.subresourceRange.levelCount = 1;
    barrier.subresourceRange.layerCount = static_cast<std::uint32_t>(layers);

    VkDependencyInfo dependency = {};
    dependency.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
    dependency.imageMemoryBarrierCount = 1;
    dependency.pImageMemoryBarriers = &barrier;

    vkCmdPipelineBarrier2(buffer, &dependency);
}

// Every level but the last back from TRANSFER_SRC, so the whole image is in
// the one layout the tracker records.
void makeChainWritable(VkCommandBuffer buffer, VkImage image, int levels, int layers)
{
    VkImageMemoryBarrier2 barrier = {};
    barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
    barrier.srcStageMask = VK_PIPELINE_STAGE_2_BLIT_BIT;
    barrier.srcAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT;
    barrier.dstStageMask = blitDestination.stage;
    barrier.dstAccessMask = blitDestination.access;
    barrier.oldLayout = imageTransferSrc.layout;
    barrier.newLayout = imageTransferDst.layout;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = image;
    barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    barrier.subresourceRange.levelCount = static_cast<std::uint32_t>(levels - 1);
    barrier.subresourceRange.layerCount = static_cast<std::uint32_t>(layers);

    VkDependencyInfo dependency = {};
    dependency.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
    dependency.imageMemoryBarrierCount = 1;
    dependency.pImageMemoryBarriers = &barrier;

    vkCmdPipelineBarrier2(buffer, &dependency);
}
} // namespace

// A vkCmdBlitImage per level with a linear filter, every layer at once and a
// volume's images as the blit's depth. The tracker holds one layout for the
// whole image, so the chain is walked with barriers of its own between
// levels and handed back in TRANSFER_DST before resting.
void recordMipGeneration(void* recording, const Texture& texture)
{
    auto* commands = recordingOutsidePasses(recording);
    auto* data = textureDataOf(texture);

    if (commands == nullptr || data == nullptr || data->mipLevels < 2)
        return;

    auto buffer = commands->buffer;
    const auto layers = data->slices;
    const auto volume = data->kind == TextureType::ThreeD;

    transitionTextureForUse(buffer, *data, blitDestination);

    for (auto level = 1; level < data->mipLevels; ++level)
    {
        makeLevelReadable(buffer, data->image, level - 1, layers);

        VkImageBlit region = {};
        region.srcSubresource = colorLayer(level - 1, 0, layers);
        region.srcOffsets[1] = {mipExtent(data->width, level - 1),
                                mipExtent(data->height, level - 1),
                                volume ? mipExtent(data->depth, level - 1) : 1};
        region.dstSubresource = colorLayer(level, 0, layers);
        region.dstOffsets[1] = {mipExtent(data->width, level),
                                mipExtent(data->height, level),
                                volume ? mipExtent(data->depth, level) : 1};

        vkCmdBlitImage(buffer,
                       data->image,
                       imageTransferSrc.layout,
                       data->image,
                       imageTransferDst.layout,
                       1,
                       &region,
                       VK_FILTER_LINEAR);
    }

    makeChainWritable(buffer, data->image, data->mipLevels, layers);

    data->use = blitDestination;

    restAfterCopy(buffer, *data);
}
} // namespace eacp::GPU
