#pragma once

#include "VulkanContext.h"

#include "../Codegen/ShaderBindings.h"
#include "../Frame/ComputePass.h"
#include "../Pipeline/RenderPipeline.h"

#include <memory>

// The concrete types the public GPU classes' opaque native handles point to.

namespace eacp::GPU
{

// One: the GLSL emitter writes exactly one uniform block.
constexpr int maxUniformSlots = 1;

constexpr int maxBufferSlots = ComputePass::maxBufferSlots;

static_assert(vulkanComputeUniformBinding
                  == ComputePass::textureRegisterBase + maxTextureSlots,
              "the compute uniform block must sit above every texture binding");

struct VulkanBufferData
{
    VkBuffer buffer = VK_NULL_HANDLE;
    VmaAllocation allocation = nullptr;
    std::size_t size = 0;

    BufferUse use;
    std::uint64_t recordingId = 0;

    // Non-null for a Streaming buffer: reads and writes are a memcpy here.
    std::byte* mapped = nullptr;
};

// Exhaustive rather than defaulted: a new format is then a -Wswitch warning.
inline VkFormat toVkFormat(TextureFormat format)
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
        case TextureFormat::BC1RGBA:
            return VK_FORMAT_BC1_RGBA_UNORM_BLOCK;
        case TextureFormat::BC2RGBA:
            return VK_FORMAT_BC2_UNORM_BLOCK;
        case TextureFormat::BC3RGBA:
            return VK_FORMAT_BC3_UNORM_BLOCK;
        case TextureFormat::BC7RGBA:
            return VK_FORMAT_BC7_UNORM_BLOCK;
    }

    return VK_FORMAT_UNDEFINED;
}

inline VkFormat toVkFormat(PixelFormat format)
{
    switch (format)
    {
        case PixelFormat::BGRA8Unorm:
            return VK_FORMAT_B8G8R8A8_UNORM;
        case PixelFormat::RGBA8Unorm:
            return VK_FORMAT_R8G8B8A8_UNORM;
        case PixelFormat::RGBA16Float:
            return VK_FORMAT_R16G16B16A16_SFLOAT;
        case PixelFormat::RGBA32Float:
            return VK_FORMAT_R32G32B32A32_SFLOAT;
        case PixelFormat::R32Float:
            return VK_FORMAT_R32_SFLOAT;
    }

    return VK_FORMAT_UNDEFINED;
}

// D32 rather than D24_UNORM_S8_UINT: precision does not change with stencil.
inline VkFormat depthAttachmentFormat(bool withStencil)
{
    return withStencil ? VK_FORMAT_D32_SFLOAT_S8_UINT : VK_FORMAT_D32_SFLOAT;
}

// A depth image with a stencil plane is transitioned on both aspects at once.
inline VkImageAspectFlags depthAspectMask(bool withStencil)
{
    if (withStencil)
        return VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT;

    return VK_IMAGE_ASPECT_DEPTH_BIT;
}

// The flag bit for a sample count is the count itself: 1, 2, 4, 8 in order.
inline VkSampleCountFlagBits toVkSampleCount(int samples)
{
    return static_cast<VkSampleCountFlagBits>(samples < 1 ? 1 : samples);
}

struct ImageUse
{
    VkImageLayout layout = VK_IMAGE_LAYOUT_UNDEFINED;
    VkPipelineStageFlags2 stage = VK_PIPELINE_STAGE_2_NONE;
    VkAccessFlags2 access = VK_ACCESS_2_NONE;

    bool operator==(const ImageUse& other) const
    {
        return layout == other.layout && stage == other.stage
               && access == other.access;
    }
};

// Barriers are illegal inside a render pass, so every image has a resting use.
inline constexpr auto imageSampled = ImageUse {
    VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
    VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
    VK_ACCESS_2_SHADER_SAMPLED_READ_BIT};

// GENERAL, Vulkan having no storage-image layout; a sampler reads it too.
inline constexpr auto imageStorage = ImageUse {
    VK_IMAGE_LAYOUT_GENERAL,
    VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
    VK_ACCESS_2_SHADER_READ_BIT | VK_ACCESS_2_SHADER_WRITE_BIT};

inline constexpr auto imageTransferSrc =
    ImageUse {VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
              VK_PIPELINE_STAGE_2_COPY_BIT,
              VK_ACCESS_2_TRANSFER_READ_BIT};

inline constexpr auto imageTransferDst =
    ImageUse {VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
              VK_PIPELINE_STAGE_2_COPY_BIT,
              VK_ACCESS_2_TRANSFER_WRITE_BIT};

inline constexpr auto imageColorAttachment = ImageUse {
    VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
    VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
    VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT};

// What vkQueuePresentKHR requires; a semaphore, not a barrier, orders it.
inline constexpr auto imagePresent =
    ImageUse {VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
              VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
              VK_ACCESS_2_NONE};

// Stamped onto a freshly acquired swapchain image. The stage must not be NONE:
// the first pass's barrier names it, ordering it behind the acquire semaphore.
inline constexpr auto imageAcquired =
    ImageUse {VK_IMAGE_LAYOUT_UNDEFINED,
              VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
              VK_ACCESS_2_NONE};

inline constexpr auto imageDepthAttachment =
    ImageUse {VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
              VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT
                  | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT,
              VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT
                  | VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT};

// The one layout legal for both aspects of a depth buffer carrying stencil.
inline constexpr auto imageDepthSampled =
    ImageUse {VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL,
              VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
              VK_ACCESS_2_SHADER_SAMPLED_READ_BIT};

// Tracked for the image's whole life: a layout survives a submission.
inline void recordImageBarrier(VkCommandBuffer commandBuffer,
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

// What Texture::nativeTexture() and nativeReadView() point to.
struct VulkanTextureData
{
    // On a multisampled target, the resolve destination.
    VkImage image = VK_NULL_HANDLE;
    VmaAllocation allocation = nullptr;

    VkFormat format = VK_FORMAT_UNDEFINED;

    int width = 0;
    int height = 0;

    int mipLevels = 1;
    bool cube = false;

    // The VkImage then belongs to the swapchain, and nothing samples it.
    bool presentable = false;

    int sampleCount = 1;

    VkImageView sampledView = VK_NULL_HANDLE;
    VkImageView attachmentView = VK_NULL_HANDLE;
    VkImageView storageView = VK_NULL_HANDLE;

    ImageUse use;

    // The companion a pass draws into, resolved into `image` at the pass's end.
    VkImage msaaImage = VK_NULL_HANDLE;
    VmaAllocation msaaAllocation = nullptr;
    VkImageView msaaView = VK_NULL_HANDLE;
    ImageUse msaaUse;

    VkImage depthImage = VK_NULL_HANDLE;
    VmaAllocation depthAllocation = nullptr;
    VkImageView depthAttachmentView = VK_NULL_HANDLE;
    ImageUse depthUse;

    bool depthHasStencil = false;
    VkFormat depthFormat = VK_FORMAT_UNDEFINED;

    // Generated shaders declare a sampler2D, not a sampler2DMS.
    VkImage resolvedDepthImage = VK_NULL_HANDLE;
    VmaAllocation resolvedDepthAllocation = nullptr;
    VkImageView resolvedDepthAttachmentView = VK_NULL_HANDLE;
    ImageUse resolvedDepthUse;

    // Depth aspect alone: Vulkan refuses a sampled view of two aspects at once.
    VkImageView depthReadView = VK_NULL_HANDLE;

    bool isValid() const
    {
        if (image == VK_NULL_HANDLE)
            return false;

        return presentable ? attachmentView != VK_NULL_HANDLE
                           : sampledView != VK_NULL_HANDLE;
    }

    bool isRenderTarget() const { return attachmentView != VK_NULL_HANDLE; }
    bool isComputeWritable() const { return storageView != VK_NULL_HANDLE; }
    bool isMultisampled() const { return msaaView != VK_NULL_HANDLE; }
    bool hasDepth() const { return depthAttachmentView != VK_NULL_HANDLE; }
    bool hasStencil() const { return hasDepth() && depthHasStencil; }
    bool hasSampleableDepth() const { return depthReadView != VK_NULL_HANDLE; }

    VkImageView colorAttachmentView() const
    {
        return isMultisampled() ? msaaView : attachmentView;
    }

    VkImageView colorResolveView() const
    {
        return isMultisampled() ? attachmentView : VK_NULL_HANDLE;
    }

    VkImage sampledDepthImage() const
    {
        return resolvedDepthImage != VK_NULL_HANDLE ? resolvedDepthImage
                                                    : depthImage;
    }

    const ImageUse& restingUse() const
    {
        if (presentable)
            return imagePresent;

        return isComputeWritable() ? imageStorage : imageSampled;
    }

    const ImageUse& depthRestingUse() const
    {
        return hasSampleableDepth() && !isMultisampled() ? imageDepthSampled
                                                         : imageDepthAttachment;
    }
};

// What a drawable Frame is handed. The view owns all but `presentResult`.
struct VulkanDrawable
{
    VkSwapchainKHR swapchain = VK_NULL_HANDLE;

    std::uint32_t imageIndex = 0;
    VulkanTextureData* target = nullptr;

    VkSemaphore acquired = VK_NULL_HANDLE;
    VkSemaphore renderFinished = VK_NULL_HANDLE;

    // VK_NOT_READY marks a frame that never presented.
    VkResult presentResult = VK_NOT_READY;
};

inline void transitionTextureForUse(VkCommandBuffer commandBuffer,
                                    VulkanTextureData& data,
                                    const ImageUse& target)
{
    recordImageBarrier(
        commandBuffer, data.image, VK_IMAGE_ASPECT_COLOR_BIT, data.use, target);
}

inline void transitionMultisampleForUse(VkCommandBuffer commandBuffer,
                                        VulkanTextureData& data,
                                        const ImageUse& target)
{
    recordImageBarrier(commandBuffer,
                       data.msaaImage,
                       VK_IMAGE_ASPECT_COLOR_BIT,
                       data.msaaUse,
                       target);
}

inline void transitionDepthForUse(VkCommandBuffer commandBuffer,
                                  VulkanTextureData& data,
                                  const ImageUse& target)
{
    recordImageBarrier(commandBuffer,
                       data.depthImage,
                       depthAspectMask(data.depthHasStencil),
                       data.depthUse,
                       target);
}

inline void transitionResolvedDepthForUse(VkCommandBuffer commandBuffer,
                                          VulkanTextureData& data,
                                          const ImageUse& target)
{
    recordImageBarrier(commandBuffer,
                       data.resolvedDepthImage,
                       depthAspectMask(data.depthHasStencil),
                       data.resolvedDepthUse,
                       target);
}

VkImageView makeVulkanImageView(VkDevice device,
                                VkImage image,
                                VkFormat viewFormat,
                                VkImageViewType type,
                                VkImageAspectFlags aspect,
                                int levels,
                                int layers);

// False means the caller must refuse the target.
bool createVulkanMultisampleCompanion(VulkanContext& context,
                                      VulkanTextureData& data);

void createVulkanDepthCompanion(VulkanContext& context,
                                VulkanTextureData& data,
                                bool withStencil,
                                bool sampleable);

// Deferred release, a command buffer possibly still naming them.
void releaseVulkanCompanions(VulkanContext& context, VulkanTextureData& data);

// Per module: one slot is a sampler2D in one shader, a writeonly image2D in
// another.
struct VulkanTextureBindings
{
    bool any() const { return declared != 0; }

    bool has(int slot) const
    {
        return slot >= 0 && slot < maxTextureSlots && (declared & (1u << slot)) != 0;
    }

    VkDescriptorType typeAt(int slot) const { return types[slot]; }

    void add(int slot, VkDescriptorType type)
    {
        if (slot < 0 || slot >= maxTextureSlots)
            return;

        declared |= 1u << slot;
        types[slot] = type;
    }

    void merge(const VulkanTextureBindings& other)
    {
        for (auto slot = 0; slot < maxTextureSlots; ++slot)
            if (other.has(slot))
                add(slot, other.typeAt(slot));
    }

    std::uint32_t declared = 0;
    VkDescriptorType types[maxTextureSlots] = {};
};

// `firstBinding` is where the texture range starts: vulkanTextureBinding(0) for
// a render shader, vulkanComputeTextureBinding(0) for a kernel.
VulkanTextureBindings spirvTextureBindings(const Vector<std::uint32_t>& words,
                                           int firstBinding);

// Built per pipeline; a kernel declaring no texture uses VulkanShared's.
PipelineLayouts makeComputeLayouts(VkDevice device,
                                   const VulkanTextureBindings& textures);

// Pointed to by ShaderLibrary::nativeLibrary().
struct VulkanShaderProgram
{
    VkShaderModule vertex = VK_NULL_HANDLE;
    VkShaderModule fragment = VK_NULL_HANDLE;
    VkShaderModule compute = VK_NULL_HANDLE;

    VulkanTextureBindings textures;
};

// Pointed to by ComputePipeline::nativeState().
struct VulkanComputePipeline
{
    VulkanTextureBindings textures;

    VkPipeline pipeline = VK_NULL_HANDLE;
    VkPipelineLayout layout = VK_NULL_HANDLE;
    VkDescriptorSetLayout setLayout = VK_NULL_HANDLE;
};

// Pointed to by RenderPipeline::nativeState(). Both stages share one uniform
// block at binding 0; binding different bytes per stage needs a second number.
struct VulkanRenderPipeline
{
    std::uint32_t strideForSlot(int slot) const
    {
        if (slot >= 0 && slot < strides.size())
            return strides[slot];

        if (!strides.empty())
            return strides[0];

        return 0;
    }

    VkPipeline pipeline = VK_NULL_HANDLE;
    VkPipelineLayout layout = VK_NULL_HANDLE;
    VkDescriptorSetLayout setLayout = VK_NULL_HANDLE;

    Vector<std::uint32_t> strides;

    // Also inside the VkPipeline, and not dynamic state here.
    VkPrimitiveTopology topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkCullModeFlags cullMode = VK_CULL_MODE_NONE;
    VkFrontFace frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;

    // What the pipeline was compiled against.
    bool depth = false;
    bool stencil = false;
    int sampleCount = 1;
    VkFormat colorFormat = VK_FORMAT_UNDEFINED;
};

struct VulkanComputeEncoder
{
    CommandContext* commands = nullptr;

    // Null and -1 when the pass is not timed.
    VkQueryPool queryPool = VK_NULL_HANDLE;
    int endQuery = -1;
};

// A null `pipeline` is the pass's own test for "no draw may be recorded".
struct VulkanRenderEncoder
{
    CommandContext* commands = nullptr;
    VulkanTextureData* target = nullptr;

    int targetWidth = 0;
    int targetHeight = 0;

    const VulkanRenderPipeline* pipeline = nullptr;

    VkQueryPool queryPool = VK_NULL_HANDLE;
    int endQuery = -1;
};

inline void recordPassEndTimestamp(CommandContext* commands,
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

inline void endTimedPass(const VulkanComputeEncoder& encoder)
{
    recordPassEndTimestamp(encoder.commands, encoder.queryPool, encoder.endQuery);
}

inline void endTimedPass(const VulkanRenderEncoder& encoder)
{
    recordPassEndTimestamp(encoder.commands, encoder.queryPool, encoder.endQuery);
}

// First use in a recording is free: every recording ends with a global barrier.
inline void transitionForUse(CommandContext& commands,
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

// Tracking without recording: a buffer barrier inside a render pass is illegal,
// and barrierBeforeRendering has already ordered everything the pass can bind.
inline void noteBufferUse(CommandContext& commands,
                          VulkanBufferData& data,
                          const BufferUse& target)
{
    if (data.buffer == VK_NULL_HANDLE)
        return;

    data.recordingId = commands.recordingId;
    data.use = target;
}

// Barriers being illegal inside a render pass, this covers everything one may
// bind - attachment stages included, two passes into one target ordering nothing.
inline void barrierBeforeRendering(VkCommandBuffer commandBuffer)
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

// Global rather than per resource: the pass does not know what the kernel wrote.
inline void barrierAfterDispatch(VkCommandBuffer commandBuffer)
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
