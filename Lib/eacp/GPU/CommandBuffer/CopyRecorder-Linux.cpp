#include "CopyRecorder.h"

#include "../Vulkan/VulkanTypes.h"

#include <eacp/Core/Utils/Logging.h>

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
} // namespace eacp::GPU
