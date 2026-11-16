#pragma once

#include "VulkanContext.h"

#include "../Codegen/ShaderBindings.h"
#include "../Frame/ComputePass.h"
#include "../Pipeline/RenderPipeline.h"

#include <memory>

// The concrete types the public GPU classes' opaque native handles point to.

namespace eacp::GPU
{
class Sampler;


// One: the GLSL emitter writes exactly one uniform block.
constexpr int maxUniformSlots = 1;

// A render stage's storage buffers, which every backend binds the same eight of.
constexpr int maxBufferSlots = ComputePass::portableBufferSlots;

constexpr int maxComputeBufferSlots = ComputePass::maxBufferSlots;

// The compute layout declares every slot, so a device whose per-stage
// storage-buffer limit is below maxComputeBufferSlots gets that many and no more.
int vulkanComputeBufferSlots(const VkPhysicalDeviceLimits& limits);

int vulkanComputeBufferSlots();

// The render layout declares all sixteen; the spec's floor for both limits is
// sixteen, so the clamp is for a device reporting less than it should.
int vulkanTextureSlots(const VkPhysicalDeviceLimits& limits);

int vulkanTextureSlots();

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
VkFormat toVkFormat(TextureFormat format);

// Through the texture format the attachment is the attachment of, so the two
// cannot disagree.
VkFormat toVkFormat(PixelFormat format);

// D32 rather than D24_UNORM_S8_UINT: precision does not change with stencil.
VkFormat depthAttachmentFormat(bool withStencil);

// A depth image with a stencil plane is transitioned on both aspects at once.
VkImageAspectFlags depthAspectMask(bool withStencil);

// The flag bit for a sample count is the count itself: 1, 2, 4, 8 in order.
VkSampleCountFlagBits toVkSampleCount(int samples);

VkCompareOp toVkCompareOp(CompareFunction compare);

// The VkSampler a made Sampler wraps.
VkSampler toVkSampler(const Sampler& sampler);

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
    VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT
        | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
    VK_ACCESS_2_SHADER_SAMPLED_READ_BIT};

// GENERAL, Vulkan having no storage-image layout; a sampler reads it too.
inline constexpr auto imageStorage = ImageUse {
    VK_IMAGE_LAYOUT_GENERAL,
    VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT
        | VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
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
// Compute among the stages, for ComputePass::setInputDepthTexture.
inline constexpr auto imageDepthSampled = ImageUse {
    VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL,
    VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT
        | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
    VK_ACCESS_2_SHADER_SAMPLED_READ_BIT};

// Tracked for the image's whole life: a layout survives a submission.
void recordImageBarrier(VkCommandBuffer commandBuffer,
                        VkImage image,
                        VkImageAspectFlags aspect,
                        ImageUse& use,
                        const ImageUse& target);

// A view of one level and a run of layers of a texture or its depth
// companion, made the first time a pass or a kernel names it: a colour or
// depth attachment of `count` layers from `slice`, or (`perLevel`) a sampled or
// storage view of one level and every slice.
struct VulkanSubresourceView
{
    enum class Use
    {
        ColorAttachment,
        DepthAttachment,
        Sampled,
        Storage,
        SampledArray,
        DepthSlice
    };

    Use use = Use::ColorAttachment;
    int level = 0;
    int slice = 0;
    int count = 1;
    VkImageView view = VK_NULL_HANDLE;
};

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

    // 2D slices (layers times six on a cube kind), and a volume's images.
    TextureType kind = TextureType::TwoD;
    int slices = 1;
    int depth = 1;

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

    // Every view past the ones above that a pass or a kernel has asked for.
    Vector<VulkanSubresourceView> subresourceViews;

    bool isValid() const;

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

    const ImageUse& restingUse() const;

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

    // Whether the swapchain was created with TRANSFER_SRC, which is what
    // Frame::copyDrawable needs (GPUView::setReadableDrawable).
    bool readable = false;

    // VK_NOT_READY marks a frame that never presented.
    VkResult presentResult = VK_NOT_READY;
};

void transitionTextureForUse(VkCommandBuffer commandBuffer,
                             VulkanTextureData& data,
                             const ImageUse& target);

void transitionMultisampleForUse(VkCommandBuffer commandBuffer,
                                 VulkanTextureData& data,
                                 const ImageUse& target);

void transitionDepthForUse(VkCommandBuffer commandBuffer,
                           VulkanTextureData& data,
                           const ImageUse& target);

void transitionResolvedDepthForUse(VkCommandBuffer commandBuffer,
                                   VulkanTextureData& data,
                                   const ImageUse& target);

struct VulkanRenderPassBegin
{
    bool isValid() const { return framebuffer != VK_NULL_HANDLE; }

    VkRenderPass renderPass = VK_NULL_HANDLE;
    VkFramebuffer framebuffer = VK_NULL_HANDLE;
    VkRect2D renderArea = {};
    Vector<VkClearValue> clearValues;
};

VulkanRenderPassBegin
    prepareVulkanRenderPass(const VkRenderingInfo& rendering,
                            std::span<VulkanTextureData* const> targets,
                            VkFormat depthFormat);

void beginVulkanRenderPass(VkCommandBuffer commandBuffer,
                           const VulkanRenderPassBegin& begin);

VkImageView makeVulkanImageView(VkDevice device,
                                VkImage image,
                                VkFormat viewFormat,
                                VkImageViewType type,
                                VkImageAspectFlags aspect,
                                int levels,
                                int layers);

// The same from a base level and layer.
VkImageView makeVulkanImageView(VkDevice device,
                                VkImage image,
                                VkFormat viewFormat,
                                VkImageViewType type,
                                VkImageAspectFlags aspect,
                                int baseLevel,
                                int levels,
                                int baseLayer,
                                int layers);

// Where a pass into `count` layers from `slice` of `level` draws and tests:
// the views made with the target for level 0 of slice 0, and a view made on
// first use otherwise, kept in subresourceViews. A volume's layers are its
// images at that level. Null when the view cannot be made.
VkImageView
    vulkanColorTargetView(VulkanTextureData& data, int level, int slice, int count);
VkImageView
    vulkanDepthTargetView(VulkanTextureData& data, int level, int slice, int count);

// One level and every slice, as the kind's sampled or storage view type.
VkImageView vulkanLevelView(VulkanTextureData& data, bool storage, int level);

// One level of a cube or a cube array, every slice, sampled as a 2D array.
// Null on any other kind; a storage view of a cube is a 2D array already.
VkImageView vulkanSampledArrayView(VulkanTextureData& data, int level);

// Level 0 of one slice of the sampleable depth plane as a 2D view, for the
// ...DepthTextureSlice binds; depthReadView for slice 0 of a 2D target. Null
// without sampleableDepth or for a slice the plane lacks.
VkImageView vulkanDepthSliceView(VulkanTextureData& data, int slice);

// Hands every view in subresourceViews to `context`'s deferred release.
void releaseVulkanSubresourceViews(VulkanContext& context, VulkanTextureData& data);

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

    // CUBE for a samplerCube, 2D for everything else the emitters write.
    VkImageViewType viewTypeAt(int slot) const { return viewTypes[slot]; }

    void add(int slot,
             VkDescriptorType type,
             VkImageViewType viewType = VK_IMAGE_VIEW_TYPE_2D);

    void merge(const VulkanTextureBindings& other);

    std::uint32_t declared = 0;
    VkDescriptorType types[maxTextureSlots] = {};
    VkImageViewType viewTypes[maxTextureSlots] = {};
};

// `firstBinding` is where the texture range starts: vulkanTextureBinding(0) for
// a render shader, vulkanComputeTextureBinding(0) for a kernel.
VulkanTextureBindings spirvTextureBindings(const Vector<std::uint32_t>& words,
                                           int firstBinding);

// Built per pipeline; a kernel declaring no texture uses VulkanShared's.
PipelineLayouts makeComputeLayouts(VkDevice device,
                                   int bufferSlots,
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
    std::uint32_t strideForSlot(int slot) const;

    VkPipeline pipeline = VK_NULL_HANDLE;
    VkPipelineLayout layout = VK_NULL_HANDLE;
    VkDescriptorSetLayout setLayout = VK_NULL_HANDLE;

    Vector<std::uint32_t> strides;

    // The texture slots the two stages declare, for the placeholders a draw
    // writes into the ones nothing was bound to.
    VulkanTextureBindings textures;

    // Also inside the VkPipeline, and not dynamic state here.
    VkPrimitiveTopology topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkCullModeFlags cullMode = VK_CULL_MODE_NONE;
    VkFrontFace frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;

    // What the pipeline was compiled against.
    bool depth = false;
    bool stencil = false;
    int sampleCount = 1;
    int colorCount = 1;
    std::array<VkFormat, maxColorAttachments> colorFormats {};
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

    // Every colour target of the pass, the first being the one whose depth is
    // attached; each goes back to its resting use when the pass ends.
    Vector<VulkanTextureData*> targets;

    int targetWidth = 0;
    int targetHeight = 0;

    const VulkanRenderPipeline* pipeline = nullptr;

    VkQueryPool queryPool = VK_NULL_HANDLE;
    int endQuery = -1;

    // The target whose depth plane is attached: targets[0], or the pass's
    // DepthTarget. Its plane goes back to its resting use with the colours.
    VulkanTextureData* depthTarget = nullptr;
};

void recordPassEndTimestamp(CommandContext* commands,
                            VkQueryPool queryPool,
                            int endQuery);

// A storage-buffer bind, whole or ranged: the shader's element zero is the
// element at the offset. The alignment the descriptor demands is a device limit
// (16 on lavapipe) rather than the four bytes Metal and D3D12 take, which is
// what Device::storageBufferOffsetAlignment reports; an offset off that grid
// binds nothing, as one at or past the end does.
VkDescriptorBufferInfo vulkanStorageBufferInfo(const VulkanBufferData* data,
                                               const BufferRange& range);

void endTimedPass(const VulkanComputeEncoder& encoder);

void endTimedPass(const VulkanRenderEncoder& encoder);

// First use in a recording is free: every recording ends with a global barrier.
void transitionForUse(CommandContext& commands,
                      VulkanBufferData& data,
                      const BufferUse& target);

// Tracking without recording: a buffer barrier inside a render pass is illegal,
// and barrierBeforeRendering has already ordered everything the pass can bind.
void noteBufferUse(CommandContext& commands,
                   VulkanBufferData& data,
                   const BufferUse& target);

// Barriers being illegal inside a render pass, this covers everything one may
// bind - attachment stages included, two passes into one target ordering nothing.
void barrierBeforeRendering(VkCommandBuffer commandBuffer);

// Global rather than per resource: the pass does not know what the kernel wrote.
void barrierAfterDispatch(VkCommandBuffer commandBuffer);

} // namespace eacp::GPU
