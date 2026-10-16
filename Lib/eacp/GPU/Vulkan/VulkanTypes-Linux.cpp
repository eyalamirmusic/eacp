#include "VulkanTypes.h"

#include "../Texture/Sampler.h"

#include <algorithm>

namespace eacp::GPU
{
VkCompareOp toVkCompareOp(CompareFunction compare)
{
    switch (compare)
    {
        case CompareFunction::Never:
            return VK_COMPARE_OP_NEVER;
        case CompareFunction::Less:
            return VK_COMPARE_OP_LESS;
        case CompareFunction::LessEqual:
            return VK_COMPARE_OP_LESS_OR_EQUAL;
        case CompareFunction::Equal:
            return VK_COMPARE_OP_EQUAL;
        case CompareFunction::NotEqual:
            return VK_COMPARE_OP_NOT_EQUAL;
        case CompareFunction::GreaterEqual:
            return VK_COMPARE_OP_GREATER_OR_EQUAL;
        case CompareFunction::Greater:
            return VK_COMPARE_OP_GREATER;
        case CompareFunction::Always:
            return VK_COMPARE_OP_ALWAYS;
    }

    return VK_COMPARE_OP_NEVER;
}

VkSampler toVkSampler(const Sampler& sampler)
{
    return reinterpret_cast<VkSampler>(sampler.nativeSampler());
}

int vulkanComputeBufferSlots(const VkPhysicalDeviceLimits& limits)
{
    const auto limit = std::min(limits.maxPerStageDescriptorStorageBuffers,
                                static_cast<std::uint32_t>(maxComputeBufferSlots));

    return std::max(static_cast<int>(limit), maxBufferSlots);
}

int vulkanComputeBufferSlots()
{
    return vulkanComputeBufferSlots(getVulkanShared().getProperties().limits);
}

int vulkanTextureSlots(const VkPhysicalDeviceLimits& limits)
{
    const auto limit =
        std::min({limits.maxPerStageDescriptorSamplers,
                  limits.maxPerStageDescriptorSampledImages,
                  static_cast<std::uint32_t>(RenderPass::maxStageTextureSlots)});

    return std::max(static_cast<int>(limit), maxTextureSlots);
}

int vulkanTextureSlots()
{
    return vulkanTextureSlots(getVulkanShared().getProperties().limits);
}

VkFormat toVkFormat(TextureFormat format)
{
    switch (format)
    {
        case TextureFormat::RGBA8Unorm:
            return VK_FORMAT_R8G8B8A8_UNORM;
        case TextureFormat::BGRA8Unorm:
            return VK_FORMAT_B8G8R8A8_UNORM;
        case TextureFormat::R8Unorm:
            return VK_FORMAT_R8_UNORM;
        case TextureFormat::RG8Unorm:
            return VK_FORMAT_R8G8_UNORM;
        case TextureFormat::RGBA16Float:
            return VK_FORMAT_R16G16B16A16_SFLOAT;
        case TextureFormat::RGBA32Float:
            return VK_FORMAT_R32G32B32A32_SFLOAT;
        case TextureFormat::R32Float:
            return VK_FORMAT_R32_SFLOAT;
        case TextureFormat::R16Float:
            return VK_FORMAT_R16_SFLOAT;
        case TextureFormat::RG16Float:
            return VK_FORMAT_R16G16_SFLOAT;
        case TextureFormat::RG32Float:
            return VK_FORMAT_R32G32_SFLOAT;
        case TextureFormat::R16Unorm:
            return VK_FORMAT_R16_UNORM;
        case TextureFormat::R32Uint:
            return VK_FORMAT_R32_UINT;
        case TextureFormat::RGBA8UnormSrgb:
            return VK_FORMAT_R8G8B8A8_SRGB;
        case TextureFormat::BGRA8UnormSrgb:
            return VK_FORMAT_B8G8R8A8_SRGB;
        case TextureFormat::RG16Unorm:
            return VK_FORMAT_R16G16_UNORM;
        case TextureFormat::RGBA16Unorm:
            return VK_FORMAT_R16G16B16A16_UNORM;
        case TextureFormat::RGB10A2Unorm:
            return VK_FORMAT_A2B10G10R10_UNORM_PACK32;
        case TextureFormat::RG11B10Float:
            return VK_FORMAT_B10G11R11_UFLOAT_PACK32;
        case TextureFormat::RGB9E5Float:
            return VK_FORMAT_E5B9G9R9_UFLOAT_PACK32;
        case TextureFormat::R8Uint:
            return VK_FORMAT_R8_UINT;
        case TextureFormat::RG8Uint:
            return VK_FORMAT_R8G8_UINT;
        case TextureFormat::RGBA8Uint:
            return VK_FORMAT_R8G8B8A8_UINT;
        case TextureFormat::R16Uint:
            return VK_FORMAT_R16_UINT;
        case TextureFormat::RG16Uint:
            return VK_FORMAT_R16G16_UINT;
        case TextureFormat::RGBA16Uint:
            return VK_FORMAT_R16G16B16A16_UINT;
        case TextureFormat::RG32Uint:
            return VK_FORMAT_R32G32_UINT;
        case TextureFormat::RGBA32Uint:
            return VK_FORMAT_R32G32B32A32_UINT;
        case TextureFormat::R8Sint:
            return VK_FORMAT_R8_SINT;
        case TextureFormat::RG8Sint:
            return VK_FORMAT_R8G8_SINT;
        case TextureFormat::RGBA8Sint:
            return VK_FORMAT_R8G8B8A8_SINT;
        case TextureFormat::R16Sint:
            return VK_FORMAT_R16_SINT;
        case TextureFormat::RG16Sint:
            return VK_FORMAT_R16G16_SINT;
        case TextureFormat::RGBA16Sint:
            return VK_FORMAT_R16G16B16A16_SINT;
        case TextureFormat::R32Sint:
            return VK_FORMAT_R32_SINT;
        case TextureFormat::RG32Sint:
            return VK_FORMAT_R32G32_SINT;
        case TextureFormat::RGBA32Sint:
            return VK_FORMAT_R32G32B32A32_SINT;
        case TextureFormat::BC1RGBA:
            return VK_FORMAT_BC1_RGBA_UNORM_BLOCK;
        case TextureFormat::BC2RGBA:
            return VK_FORMAT_BC2_UNORM_BLOCK;
        case TextureFormat::BC3RGBA:
            return VK_FORMAT_BC3_UNORM_BLOCK;
        case TextureFormat::BC7RGBA:
            return VK_FORMAT_BC7_UNORM_BLOCK;
        case TextureFormat::BC1RGBASrgb:
            return VK_FORMAT_BC1_RGBA_SRGB_BLOCK;
        case TextureFormat::BC2RGBASrgb:
            return VK_FORMAT_BC2_SRGB_BLOCK;
        case TextureFormat::BC3RGBASrgb:
            return VK_FORMAT_BC3_SRGB_BLOCK;
        case TextureFormat::BC7RGBASrgb:
            return VK_FORMAT_BC7_SRGB_BLOCK;
        case TextureFormat::BC4RUnorm:
            return VK_FORMAT_BC4_UNORM_BLOCK;
        case TextureFormat::BC4RSnorm:
            return VK_FORMAT_BC4_SNORM_BLOCK;
        case TextureFormat::BC5RGUnorm:
            return VK_FORMAT_BC5_UNORM_BLOCK;
        case TextureFormat::BC5RGSnorm:
            return VK_FORMAT_BC5_SNORM_BLOCK;
        case TextureFormat::BC6HRGBUfloat:
            return VK_FORMAT_BC6H_UFLOAT_BLOCK;
    }

    return VK_FORMAT_UNDEFINED;
}

VkFormat toVkFormat(PixelFormat format)
{
    return toVkFormat(textureFormatFor(format));
}

VkFormat depthAttachmentFormat(bool withStencil)
{
    return withStencil ? VK_FORMAT_D32_SFLOAT_S8_UINT : VK_FORMAT_D32_SFLOAT;
}

VkImageAspectFlags depthAspectMask(bool withStencil)
{
    if (withStencil)
        return VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT;

    return VK_IMAGE_ASPECT_DEPTH_BIT;
}

VkSampleCountFlagBits toVkSampleCount(int samples)
{
    return static_cast<VkSampleCountFlagBits>(samples < 1 ? 1 : samples);
}

void recordImageBarrier(VkCommandBuffer commandBuffer,
                        VkImage image,
                        VkImageAspectFlags aspect,
                        ImageUse& use,
                        const ImageUse& target)
{
    if (image == VK_NULL_HANDLE || commandBuffer == VK_NULL_HANDLE || use == target)
        return;

    VkImageMemoryBarrier2 barrier = {};
    barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
    barrier.srcStageMask = use.stage;
    barrier.srcAccessMask = use.access;
    barrier.dstStageMask = target.stage;
    barrier.dstAccessMask = target.access;
    barrier.oldLayout = use.layout;
    barrier.newLayout = target.layout;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = image;
    barrier.subresourceRange.aspectMask = aspect;
    barrier.subresourceRange.levelCount = VK_REMAINING_MIP_LEVELS;
    barrier.subresourceRange.layerCount = VK_REMAINING_ARRAY_LAYERS;

    VkDependencyInfo dependency = {};
    dependency.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
    dependency.imageMemoryBarrierCount = 1;
    dependency.pImageMemoryBarriers = &barrier;

    vkCmdPipelineBarrier2(commandBuffer, &dependency);

    use = target;
}

void transitionTextureForUse(VkCommandBuffer commandBuffer,
                             VulkanTextureData& data,
                             const ImageUse& target)
{
    recordImageBarrier(
        commandBuffer, data.image, VK_IMAGE_ASPECT_COLOR_BIT, data.use, target);
}

void transitionMultisampleForUse(VkCommandBuffer commandBuffer,
                                 VulkanTextureData& data,
                                 const ImageUse& target)
{
    recordImageBarrier(commandBuffer,
                       data.msaaImage,
                       VK_IMAGE_ASPECT_COLOR_BIT,
                       data.msaaUse,
                       target);
}

void transitionDepthForUse(VkCommandBuffer commandBuffer,
                           VulkanTextureData& data,
                           const ImageUse& target)
{
    recordImageBarrier(commandBuffer,
                       data.depthImage,
                       depthAspectMask(data.depthHasStencil),
                       data.depthUse,
                       target);
}

void transitionResolvedDepthForUse(VkCommandBuffer commandBuffer,
                                   VulkanTextureData& data,
                                   const ImageUse& target)
{
    recordImageBarrier(commandBuffer,
                       data.resolvedDepthImage,
                       depthAspectMask(data.depthHasStencil),
                       data.resolvedDepthUse,
                       target);
}

bool VulkanTextureData::isValid() const
{
    if (image == VK_NULL_HANDLE)
        return false;

    return presentable ? attachmentView != VK_NULL_HANDLE
                       : sampledView != VK_NULL_HANDLE;
}

const ImageUse& VulkanTextureData::restingUse() const
{
    if (presentable)
        return imagePresent;

    return isComputeWritable() ? imageStorage : imageSampled;
}

void VulkanTextureBindings::add(int slot,
                                VkDescriptorType type,
                                VkImageViewType viewType)
{
    if (slot < 0 || slot >= maxTextureSlots)
        return;

    declared |= 1u << slot;
    types[slot] = type;
    viewTypes[slot] = viewType;
}

void VulkanTextureBindings::merge(const VulkanTextureBindings& other)
{
    for (auto slot = 0; slot < maxTextureSlots; ++slot)
        if (other.has(slot))
            add(slot, other.typeAt(slot), other.viewTypeAt(slot));
}

std::uint32_t VulkanRenderPipeline::strideForSlot(int slot) const
{
    if (slot >= 0 && slot < strides.size())
        return strides[slot];

    if (!strides.empty())
        return strides[0];

    return 0;
}

void recordPassEndTimestamp(CommandContext* commands,
                            VkQueryPool queryPool,
                            int endQuery)
{
    if (queryPool == VK_NULL_HANDLE || endQuery < 0 || commands == nullptr)
        return;

    vkCmdWriteTimestamp2(commands->buffer,
                         VK_PIPELINE_STAGE_2_BOTTOM_OF_PIPE_BIT,
                         queryPool,
                         static_cast<std::uint32_t>(endQuery));
}

VkDescriptorBufferInfo vulkanStorageBufferInfo(const VulkanBufferData* data,
                                               const BufferRange& range)
{
    if (data == nullptr || data->buffer == VK_NULL_HANDLE || range.offset < 0
        || static_cast<std::size_t>(range.offset) >= data->size)
        return {};

    const auto offset = static_cast<VkDeviceSize>(range.offset);
    const auto alignment =
        getVulkanShared().getProperties().limits.minStorageBufferOffsetAlignment;

    if (alignment != 0 && offset % alignment != 0)
        return {};

    return {data->buffer, offset, VK_WHOLE_SIZE};
}

void endTimedPass(const VulkanComputeEncoder& encoder)
{
    recordPassEndTimestamp(encoder.commands, encoder.queryPool, encoder.endQuery);
}

void endTimedPass(const VulkanRenderEncoder& encoder)
{
    recordPassEndTimestamp(encoder.commands, encoder.queryPool, encoder.endQuery);
}

void transitionForUse(CommandContext& commands,
                      VulkanBufferData& data,
                      const BufferUse& target)
{
    if (data.buffer == VK_NULL_HANDLE)
        return;

    if (data.recordingId != commands.recordingId)
    {
        data.recordingId = commands.recordingId;
        data.use = target;
        return;
    }

    if (data.use == target)
        return;

    VkBufferMemoryBarrier2 barrier = {};
    barrier.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2;
    barrier.srcStageMask = data.use.stage;
    barrier.srcAccessMask = data.use.access;
    barrier.dstStageMask = target.stage;
    barrier.dstAccessMask = target.access;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.buffer = data.buffer;
    barrier.size = VK_WHOLE_SIZE;

    VkDependencyInfo dependency = {};
    dependency.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
    dependency.bufferMemoryBarrierCount = 1;
    dependency.pBufferMemoryBarriers = &barrier;

    vkCmdPipelineBarrier2(commands.buffer, &dependency);

    data.use = target;
}

void noteBufferUse(CommandContext& commands,
                   VulkanBufferData& data,
                   const BufferUse& target)
{
    if (data.buffer == VK_NULL_HANDLE)
        return;

    data.recordingId = commands.recordingId;
    data.use = target;
}

void barrierBeforeRendering(VkCommandBuffer commandBuffer)
{
    constexpr auto attachmentStages = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT
                                      | VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT
                                      | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT;

    VkMemoryBarrier2 barrier = {};
    barrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2;
    barrier.srcStageMask = VK_PIPELINE_STAGE_2_COPY_BIT
                           | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT
                           | attachmentStages;
    barrier.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT
                            | VK_ACCESS_2_SHADER_WRITE_BIT
                            | VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT
                            | VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    barrier.dstStageMask =
        VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT | VK_PIPELINE_STAGE_2_VERTEX_INPUT_BIT
        | VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT
        | VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT | attachmentStages;
    barrier.dstAccessMask =
        VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT;

    VkDependencyInfo dependency = {};
    dependency.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
    dependency.memoryBarrierCount = 1;
    dependency.pMemoryBarriers = &barrier;

    vkCmdPipelineBarrier2(commandBuffer, &dependency);
}

void barrierAfterDispatch(VkCommandBuffer commandBuffer)
{
    VkMemoryBarrier2 barrier = {};
    barrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2;
    barrier.srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
    barrier.srcAccessMask = VK_ACCESS_2_SHADER_WRITE_BIT;
    barrier.dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    barrier.dstAccessMask =
        VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT;

    VkDependencyInfo dependency = {};
    dependency.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
    dependency.memoryBarrierCount = 1;
    dependency.pMemoryBarriers = &barrier;

    vkCmdPipelineBarrier2(commandBuffer, &dependency);
}
} // namespace eacp::GPU
