#pragma once

#include "VulkanContext.h"

#include "../Codegen/ShaderBindings.h"
#include "../Frame/ComputePass.h"
#include "../Pipeline/RenderPipeline.h"

#include <memory>

// Internal shared types for the Linux/Vulkan GPU backend. The public GPU
// classes expose opaque void* handles (nativeBuffer/nativeLibrary/nativeState/
// ...); these structs are what those handles point to, so the separate
// translation units agree on the concrete layout without leaking Vulkan types
// into the public headers. The D3D12 sibling is Windows/D3D12Types.h. Not part
// of GPU.h.

namespace eacp::GPU
{

// How many uniform blocks one shader may bind. One, where D3D12 declares two:
// the GLSL emitter writes exactly one interface block, at
// vulkanUniformBinding for a render shader and at
// vulkanComputeUniformBinding for a kernel (Codegen/ShaderBindings.h), and a
// second would need a binding the emitter has no number for. setBytes on a
// higher slot binds nowhere and is dropped, which is what the other two
// backends do with a slot past their own ceiling.
constexpr int maxUniformSlots = 1;

constexpr int maxBufferSlots = ComputePass::maxBufferSlots;

// The compute set is laid out in the order ShaderBindings.h prints: storage
// buffers from binding 0, textures above every buffer slot, the uniform block
// on top of both. This holds the last of those to the first two.
static_assert(vulkanComputeUniformBinding
                  == ComputePass::textureRegisterBase + maxTextureSlots,
              "the compute uniform block must sit above every texture binding");

// What Buffer::nativeBuffer() points to. `use` tracks what the buffer was last
// used as within the current recording, so a barrier is only recorded when one
// recording uses the same buffer two ways: the first use in a recording is free
// because the previous recording ended with a global barrier (see
// VulkanContext::submit).
struct VulkanBufferData
{
    VkBuffer buffer = VK_NULL_HANDLE;
    VmaAllocation allocation = nullptr;
    std::size_t size = 0;

    BufferUse use;
    std::uint64_t recordingId = 0;

    // The persistent mapping of a BufferStorage::Streaming buffer, and the one
    // test that tells the two shapes apart: non-null means a write is a memcpy
    // here and a read comes back out of the same bytes, with nothing recorded
    // either way.
    std::byte* mapped = nullptr;
};

// Format translation, in one place so the image a Texture creates, the
// attachment a RenderPipeline is compiled against and the copy a read-back
// records all name the same VkFormat. Exhaustive rather than defaulted, for
// the reason pixelFormatFor gives: a format added without a Vulkan spelling is
// a -Wswitch warning here rather than an image silently created as UNDEFINED.
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

// The one format a depth attachment is created, cleared, viewed and compiled
// against. One place because those four have to name the same value: an image,
// its view, the pass's clear and the pipeline's depthAttachmentFormat
// disagreeing is a validation error at the draw rather than at creation.
//
// D32 with or without an S8 plane rather than D24_UNORM_S8_UINT, so the depth
// keeps the same 32-bit float precision, and the same near/far behaviour,
// whether or not the stencil plane is there - and so it matches Metal, whose
// Apple-silicon devices do not have the 24-bit combined format at all, and
// D3D12's D32_FLOAT_S8X24_UINT.
inline VkFormat depthAttachmentFormat(bool withStencil)
{
    return withStencil ? VK_FORMAT_D32_SFLOAT_S8_UINT : VK_FORMAT_D32_SFLOAT;
}

// Which planes of that attachment a barrier, a clear or a view names. A depth
// image with a stencil plane must be transitioned on both aspects at once.
inline VkImageAspectFlags depthAspectMask(bool withStencil)
{
    if (withStencil)
        return VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT;

    return VK_IMAGE_ASPECT_DEPTH_BIT;
}

// A sample count as the flag bit an image, a pipeline and a resolve are all
// created with - which is the count itself, the bits being 1, 2, 4, 8 and so on
// in value order. Here rather than in one backend file because the texture that
// creates the attachment and the pipeline compiled against it have to name the
// same bit; Device::supportsSampleCount has already refused anything that is
// not a power of two in range.
inline VkSampleCountFlagBits toVkSampleCount(int samples)
{
    return static_cast<VkSampleCountFlagBits>(samples < 1 ? 1 : samples);
}

// What an image is being used as, for the barrier that has to precede it. One
// combined {layout, stage, access} rather than three separate fields, for the
// reason BufferUse combines two: every use eacp makes of an image pins all
// three together, and naming them apart invites a barrier that moves the layout
// without ordering the access.
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

// Where a colour image rests between passes. The resting layout is a rule
// rather than an accident: Vulkan forbids barriers inside vkCmdBeginRendering,
// so a pass cannot move an image while it is running - it has to find every
// attachment and every sampled image in a known layout, move them in at begin
// and put them back at end.
//
// SHADER_READ_ONLY_OPTIMAL is that layout for an ordinary texture, which is why
// an upload leaves the image here the moment the copy is recorded rather than
// waiting for the first bind.
inline constexpr auto imageSampled = ImageUse {
    VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
    VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
    VK_ACCESS_2_SHADER_SAMPLED_READ_BIT};

// GENERAL, Vulkan having no storage-image layout of its own - and the resting
// layout of a computeWrite texture for exactly that reason. A sampler reads
// GENERAL perfectly well, so a texture a kernel writes and a pass then samples
// stays here throughout instead of paying a pair of barriers a frame to visit
// SHADER_READ_ONLY_OPTIMAL and come back.
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

// What a pass moves its colour attachment to at begin - and its resolve
// destination too. A dynamic-rendering resolve names a layout for both sides
// and COLOR_ATTACHMENT_OPTIMAL is legal for each, so the multisampled companion
// and the single-sampled texture it resolves into take the same use and the
// pass records one barrier per image rather than two.
inline constexpr auto imageColorAttachment = ImageUse {
    VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
    VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
    VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT};

// The depth twin of the above, on both fragment-test stages because the depth
// write can happen at either.
inline constexpr auto imageDepthAttachment =
    ImageUse {VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
              VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT
                  | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT,
              VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT
                  | VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT};

// Where a sampleable depth buffer rests, and what a bind through
// RenderPass::setFragmentDepthTexture needs it in. The combined layout rather
// than SHADER_READ_ONLY_OPTIMAL because a depth image with a stencil plane is
// transitioned on both aspects at once (depthAspectMask), and this one is legal
// for both.
inline constexpr auto imageDepthSampled =
    ImageUse {VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL,
              VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
              VK_ACCESS_2_SHADER_SAMPLED_READ_BIT};

// Records the barrier one image needs before being used this way, and remembers
// what it is now being used as - the image sibling of transitionForUse.
//
// **There is no first-use-is-free rule here**, which is the one place this
// differs from the buffer version. A buffer's contents survive a recording
// boundary and the global barrier at the end of every submission makes them
// visible; an image also has a *layout*, and the layout the previous submission
// left it in is the layout it is still in. So the tracking runs for the image's
// whole life rather than per recording, exactly as the D3D12 backend tracks a
// resource's state for the same reason.
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

    // A layout the image has never been in yet is entered from UNDEFINED, and
    // UNDEFINED discards whatever was there - which is right for a texture
    // nothing has written, and would be wrong for one something has. The
    // tracking is what keeps those apart: `use` starts UNDEFINED and stops
    // being UNDEFINED the moment anything writes the image.
    VkDependencyInfo dependency = {};
    dependency.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
    dependency.imageMemoryBarrierCount = 1;
    dependency.pImageMemoryBarriers = &barrier;

    vkCmdPipelineBarrier2(commandBuffer, &dependency);

    use = target;
}

// What Texture::nativeTexture() and nativeReadView() point to - one struct
// carrying every image a render target is made of, the way D3D12TextureData
// does and for the same reason: nativeDepthTexture, nativeMultisampleTexture
// and nativeResolvedDepthTexture are all null on this backend, and a pass
// reaches those images through here.
//
// **What a pass has to do with it**, since the pass is written elsewhere:
//
//  - Colour. Attach `colorAttachmentView()`, which is the multisampled
//    companion when there is one and the texture itself otherwise. On a
//    multisampled target set the attachment's resolveImageView to
//    `colorResolveView()` (the texture) with VK_RESOLVE_MODE_AVERAGE_BIT, so
//    what the texture holds after the pass is always the resolved picture and
//    nothing sampling a render target has to know whether it multisamples.
//    Move both images to `imageColorAttachment` before vkCmdBeginRendering, and
//    move the *texture* back to `restingUse()` after vkCmdEndRendering. The
//    multisampled companion stays where it is: it is created with
//    COLOR_ATTACHMENT usage and nothing else, and SHADER_READ_ONLY_OPTIMAL is
//    only legal for an image created SAMPLED, so `imageColorAttachment` is
//    where that one rests. What orders two passes over it, the layout no longer
//    changing between them, is barrierBeforeRendering.
//
//  - Depth. Attach `depthAttachmentView` with aspect
//    `depthAspectMask(depthHasStencil)`, at `imageDepthAttachment`. On a
//    multisampled target that also wants its depth sampled, set the depth
//    attachment's resolveImageView to `resolvedDepthAttachmentView` with
//    VK_RESOLVE_MODE_SAMPLE_ZERO_BIT - which is what Metal's depth resolve
//    does, so the two backends hand a shader the same value, and is why this
//    backend needs none of the shader fallback D3D12 has.
//
//  - Resting layouts. Barriers cannot be issued inside vkCmdBeginRendering, so
//    every image has to be somewhere a pass can move it *from*. Colour images
//    rest at `restingUse()`; the depth attachment rests at
//    `depthRestingUse()`, and the resolved depth - which exists only because
//    something samples it - rests at `imageDepthSampled`. An upload leaves the
//    colour image at `restingUse()` the moment the copy is recorded, so a pass
//    never has to guess.
struct VulkanTextureData
{
    // The texture itself: the image a shader samples, a read-back reads and -
    // on a single-sampled target - a pass renders into. On a multisampled one
    // it is the resolve destination instead, which is what keeps
    // `sampleCount > 1` invisible to everything that only wants the picture.
    VkImage image = VK_NULL_HANDLE;
    VmaAllocation allocation = nullptr;

    // The colour format every view, copy and pipeline over this texture names.
    // Kept because a VkImage will not hand its own back.
    VkFormat format = VK_FORMAT_UNDEFINED;

    int width = 0;
    int height = 0;

    // The image's own level count, which is 1 unless a chain was built or
    // supplied. The sampled view covers all of them; every attachment view
    // covers level 0 alone, a pass having nowhere to say which level it draws.
    int mipLevels = 1;

    // Six array layers under a CUBE view rather than one, which changes the
    // sampled view's type, the layer each upload lands on, and nothing else.
    bool cube = false;

    // How many samples a pass into this target takes: 1 unless msaaImage is
    // there, and then the count it and the depth companion were created at.
    int sampleCount = 1;

    // All levels and all layers, VK_IMAGE_VIEW_TYPE_CUBE on a cube and 2D
    // otherwise. What a fragment or compute bind writes into a
    // COMBINED_IMAGE_SAMPLER descriptor.
    VkImageView sampledView = VK_NULL_HANDLE;

    // Level 0, one layer, 2D - the view a pass attaches, and on a multisampled
    // target the resolve destination rather than the attachment. Null on a
    // texture that is not a render target, which is what isRenderTarget() is.
    VkImageView attachmentView = VK_NULL_HANDLE;

    // Level 0, 2D, for a STORAGE_IMAGE descriptor. Null unless the texture was
    // created computeWrite *and* the device reported a storage-image feature
    // bit for the format, which is what isComputeWritable() answers.
    VkImageView storageView = VK_NULL_HANDLE;

    ImageUse use;

    // The multisampled colour companion a pass actually renders into, resolved
    // into `image` at the end of every pass. Its view is an attachment view on
    // the same terms as the one above. Both exist or neither does: a target
    // that got the image and not the view is refused at creation rather than
    // rendering single-sampled without saying so.
    VkImage msaaImage = VK_NULL_HANDLE;
    VmaAllocation msaaAllocation = nullptr;
    VkImageView msaaView = VK_NULL_HANDLE;
    ImageUse msaaUse;

    // The depth buffer a pass into this target attaches, at the target's own
    // sample count - both APIs require every attachment of one pass to agree on
    // it. Created with depthAttachmentFormat(stencil), viewed on
    // depthAspectMask(stencil).
    VkImage depthImage = VK_NULL_HANDLE;
    VmaAllocation depthAllocation = nullptr;
    VkImageView depthAttachmentView = VK_NULL_HANDLE;
    ImageUse depthUse;

    // Whether that buffer carries a stencil plane, which decides both the
    // aspects a barrier names and whether a pipeline drawing here must set
    // RenderPipelineDescriptor::stencil - and depthFormat, which is
    // depthAttachmentFormat of it, kept so the pass and the pipeline can name
    // the same value without recomputing it.
    bool depthHasStencil = false;
    VkFormat depthFormat = VK_FORMAT_UNDEFINED;

    // The single-sampled buffer that depth resolves into on a multisampled
    // target, and the one a shader then reads. Null everywhere else, where the
    // attachment is already single-sampled and is itself what gets read.
    //
    // It exists because a shader eacp generates declares a sampler2D, not a
    // sampler2DMS with a sample index to choose between - see
    // TextureDescriptor::sampleCount.
    VkImage resolvedDepthImage = VK_NULL_HANDLE;
    VmaAllocation resolvedDepthAllocation = nullptr;
    VkImageView resolvedDepthAttachmentView = VK_NULL_HANDLE;
    ImageUse resolvedDepthUse;

    // The depth-only read view a fragment bind hands over, over whichever of
    // the two depth images sampledDepthImage() names. Depth aspect alone even
    // where the buffer carries stencil: Vulkan refuses a sampled view of two
    // aspects at once, and a shader eacp generates reads the depth.
    //
    // Null unless TextureDescriptor::sampleableDepth asked for it, which is
    // what hasSampleableDepth() answers.
    VkImageView depthReadView = VK_NULL_HANDLE;

    bool isValid() const
    {
        return image != VK_NULL_HANDLE && sampledView != VK_NULL_HANDLE;
    }

    bool isRenderTarget() const { return attachmentView != VK_NULL_HANDLE; }
    bool isComputeWritable() const { return storageView != VK_NULL_HANDLE; }
    bool isMultisampled() const { return msaaView != VK_NULL_HANDLE; }
    bool hasDepth() const { return depthAttachmentView != VK_NULL_HANDLE; }
    bool hasStencil() const { return hasDepth() && depthHasStencil; }
    bool hasSampleableDepth() const { return depthReadView != VK_NULL_HANDLE; }

    // Where a pass into this target draws, and what it clears: the multisample
    // companion when there is one, and the texture itself otherwise.
    VkImageView colorAttachmentView() const
    {
        return isMultisampled() ? msaaView : attachmentView;
    }

    // What that attachment resolves into, and null when there is nothing to
    // resolve - which is the same test the attachment above makes, spelled the
    // other way round so a pass can hand both straight to
    // VkRenderingAttachmentInfo.
    VkImageView colorResolveView() const
    {
        return isMultisampled() ? attachmentView : VK_NULL_HANDLE;
    }

    // The depth image a shader reads, which is the resolve on a multisampled
    // target and the attachment on every other. The sibling of D3D12's
    // sampledDepthResource, and what depthReadView views.
    VkImage sampledDepthImage() const
    {
        return resolvedDepthImage != VK_NULL_HANDLE ? resolvedDepthImage
                                                    : depthImage;
    }

    // Where the colour image rests between passes. GENERAL for a texture a
    // kernel writes, because a sampler reads GENERAL too and the round trip
    // would buy nothing; SHADER_READ_ONLY_OPTIMAL for every other.
    const ImageUse& restingUse() const
    {
        return isComputeWritable() ? imageStorage : imageSampled;
    }

    // Where the depth attachment rests: readable when it is the buffer a shader
    // samples, and an attachment otherwise. A multisampled target's attachment
    // is never the sampled one - the resolve is - so it rests as an attachment
    // whatever sampleableDepth said.
    const ImageUse& depthRestingUse() const
    {
        return hasSampleableDepth() && !isMultisampled() ? imageDepthSampled
                                                         : imageDepthAttachment;
    }
};

// The four transitions the images of one texture take, each remembering its own
// use. Four functions rather than one with a selector, because which image a
// call means is the whole of what the caller is saying.
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

// What one shader declares in the texture range of its descriptor set: which
// slots are there at all, and the descriptor type each of them needs.
//
// **The type has to be per module, and it is the one binding question this
// backend has that the other two do not.** The binding map gives a texture slot
// one number whether the shader samples it or writes it (Codegen/
// ShaderBindings.h, matching the Metal indices), but Vulkan gives one binding
// one descriptor type: the GLSL emitter writes a sampled slot as a `sampler2D`
// (a COMBINED_IMAGE_SAMPLER, the sampler travelling with the image) and a
// written one as a `writeonly image2D` (a STORAGE_IMAGE). A layout shared by
// every shader would have to pick one and be wrong for the other, so each
// pipeline builds its own from what its modules were found to declare.
struct VulkanTextureBindings
{
    bool any() const { return declared != 0; }

    bool has(int slot) const
    {
        return slot >= 0 && slot < maxTextureSlots && (declared & (1u << slot)) != 0;
    }

    // The type the layout gave this slot, and therefore the only type a
    // descriptor write to it may name. Meaningless where has() is false.
    VkDescriptorType typeAt(int slot) const { return types[slot]; }

    void add(int slot, VkDescriptorType type)
    {
        if (slot < 0 || slot >= maxTextureSlots)
            return;

        declared |= 1u << slot;
        types[slot] = type;
    }

    // Merged rather than replaced, because a render pipeline's two stages are
    // two modules over one set: the vertex stage declares nothing here and the
    // fragment stage declares everything, and a pipeline built from both wants
    // the union.
    void merge(const VulkanTextureBindings& other)
    {
        for (auto slot = 0; slot < maxTextureSlots; ++slot)
            if (other.has(slot))
                add(slot, other.typeAt(slot));
    }

    std::uint32_t declared = 0;
    VkDescriptorType types[maxTextureSlots] = {};
};

// The descriptor type of every texture binding one SPIR-V module declares,
// found by reflecting the module rather than by trusting the graph that
// produced it - the pipeline layout has to describe the SPIR-V the driver is
// actually given, and the two would be one assumption apart otherwise.
//
// `firstBinding` is where the texture range starts for this kind of shader:
// vulkanTextureBinding(0) for a render shader, vulkanComputeTextureBinding(0)
// for a kernel. Bindings outside [firstBinding, firstBinding + maxTextureSlots)
// are ignored, those being the buffers and the uniform block.
VulkanTextureBindings spirvTextureBindings(const Vector<std::uint32_t>& words,
                                           int firstBinding);

// The descriptor-set layout a kernel binds through and the pipeline layout over
// it, laid out exactly as Codegen/ShaderBindings.h prints: storage buffers from
// binding 0, then one binding for each texture slot `textures` declares - at
// the descriptor type it was declared with - and the uniform block above both.
//
// Built per pipeline rather than once because the texture range is per module.
// A kernel that declares no texture wants the same layout as every other such
// kernel, which is the one VulkanShared::getComputeLayouts already holds, so
// only a kernel that binds one pays for a layout of its own.
PipelineLayouts makeComputeLayouts(VkDevice device,
                                   const VulkanTextureBindings& textures);

// Result of compiling a ShaderSource: one VkShaderModule per stage that the
// source carried, and what the modules were found to declare.
//
// Pointed to by ShaderLibrary::nativeLibrary().
struct VulkanShaderProgram
{
    VkShaderModule vertex = VK_NULL_HANDLE;
    VkShaderModule fragment = VK_NULL_HANDLE;
    VkShaderModule compute = VK_NULL_HANDLE;

    // The union of what every stage declares in the texture range, which is
    // what a pipeline built from this program lays its descriptor set out from.
    VulkanTextureBindings textures;
};

// A compiled compute pipeline, the two layouts a pass needs to bind through it,
// and what the kernel declared in the texture range of that set - which the
// pass needs at the dispatch, a descriptor write having to name the type the
// layout gave the binding. Pointed to by ComputePipeline::nativeState().
struct VulkanComputePipeline
{
    VulkanTextureBindings textures;

    VkPipeline pipeline = VK_NULL_HANDLE;
    VkPipelineLayout layout = VK_NULL_HANDLE;
    VkDescriptorSetLayout setLayout = VK_NULL_HANDLE;
};

// A compiled graphics pipeline and everything a render pass has to know about
// it that is not inside the VkPipeline. Pointed to by
// RenderPipeline::nativeState().
//
// **What the shader on the other end of `layout` declares.** The GLSL emitter
// writes a render shader as one string with both stages in it, and it writes
// every resource *outside* the EACP_VERTEX / EACP_FRAGMENT guards - so the
// vertex and the fragment module of one program declare the same bindings, and
// the render set layout makes all of them VERTEX|FRAGMENT visible:
//
//   binding vulkanUniformBinding (0)          the single std140 uniform block,
//                                             `uniforms`, as a
//                                             UNIFORM_BUFFER_DYNAMIC
//   bindings vulkanTextureBinding(0..7)       sampler2D / samplerCube, as
//                                             COMBINED_IMAGE_SAMPLERs - the
//                                             sampler travels in the write, not
//                                             in the layout
//   bindings vulkanBufferBinding(0..7)        std430 `buffer` blocks, as
//                                             STORAGE_BUFFERs, read-only
//
// **There is one uniform block, and both stages read it.** That is the one
// thing this backend does not inherit from the other two. Metal gives each
// stage its own argument table, so setVertexBytes and setFragmentBytes write
// two different buffer(uniformBase) slots; D3D12 gives them two root parameters
// over the same cbuffer register. Here they are one binding - the emitter has
// exactly one uniform block and one binding number for it (maxUniformSlots is
// 1), and a second write to binding 0 replaces the first.
//
// That is safe for everything in the tree, because nothing calls the two
// setters against each other: RenderPass::setUniforms is the only caller, it
// binds the *same* packed block to whichever stages read it, and no test, no
// Apps/GPU example and nothing in GPUWidgets or Sprites calls setFragmentBytes
// with bytes the vertex stage did not also get. So the render pass may treat
// the two as one descriptor write of the dynamic uniform at binding 0, and
// whichever setter runs last wins with identical bytes. What it must not do is
// assume the two are independent: a caller that ever binds different blocks to
// the two stages needs a second binding number in ShaderBindings.h first.
struct VulkanRenderPipeline
{
    // The single "stride for a bound slot" rule: if the slot has an explicit
    // stride use it; otherwise fall back to slot 0's, so a legacy single-buffer
    // pipeline - which builds a one-entry table - still binds correctly when
    // the caller passes a non-zero slot. Lifted from Windows/D3D12Types.h so
    // the two backends cannot drift on it.
    //
    // Read only by RenderPipeline itself, where the other two backends read it
    // at the *bind*: a Vulkan stride lives in the pipeline's
    // VkVertexInputBindingDescription, so vkCmdBindVertexBuffers takes an offset
    // and nothing else and RenderPass::setVertexBuffer has no use for this.
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

    // All three are inside the VkPipeline: topology in the input assembly, the
    // other two in the rasterizer. Reported here for the record, and to say
    // what a pass need not do - cull mode and front face are not dynamic state
    // on this backend, so there is nothing for vkCmdSetCullMode to override.
    // See makeRasterizationState in RenderPipeline-Linux.cpp.
    VkPrimitiveTopology topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkCullModeFlags cullMode = VK_CULL_MODE_NONE;
    VkFrontFace frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;

    // What the pipeline was compiled against, so a pass can check that its
    // attachments agree before recording a draw the driver would reject.
    bool depth = false;
    bool stencil = false;
    int sampleCount = 1;
    VkFormat colorFormat = VK_FORMAT_UNDEFINED;
};

// Carries the recording from CommandBuffer::beginCompute to the ComputePass.
// The CommandContext stays owned by the CommandBuffer, which submits on
// commit(); the encoder is owned by the pass.
struct VulkanComputeEncoder
{
    CommandContext* commands = nullptr;

    // Where a timed pass writes its closing timestamp. The opening one is
    // recorded by whoever began the pass; this one has to wait for the pass to
    // end, which is the pass's own business. Null and -1 when the pass carries
    // no label and is therefore not timed.
    VkQueryPool queryPool = VK_NULL_HANDLE;
    int endQuery = -1;
};

// The render sibling, carrying what RenderPass needs that a compute pass does
// not: the target, so the pass can put its attachments back where they rest
// once vkCmdEndRendering has made barriers legal again, and the target's size,
// which the full-target scissor and the clamp both work from.
//
// The bound pipeline is here rather than in RenderPass::Native because what a
// draw needs from it - the pipeline layout to bind its descriptor set through,
// and the set layout to allocate one against - is on the VulkanRenderPipeline,
// and null is the pass's own test for "no draw may be recorded".
struct VulkanRenderEncoder
{
    CommandContext* commands = nullptr;

    // The texture the pass renders into. Never null on a real encoder: a pass
    // with no target is handed no encoder at all.
    VulkanTextureData* target = nullptr;

    int targetWidth = 0;
    int targetHeight = 0;

    const VulkanRenderPipeline* pipeline = nullptr;

    VkQueryPool queryPool = VK_NULL_HANDLE;
    int endQuery = -1;
};

// Closes a timed pass, wherever the pass happens to end. One body, two kinds:
// the query pair is the whole of what timing is on this backend, and a render
// pass and a compute pass close it identically.
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

// Records the barrier a buffer needs before being used this way, and remembers
// what it is now being used as.
//
// First use in a recording is free: every recording ends with a global barrier,
// so whatever an earlier submission wrote is already visible to whatever this
// one does first. What is left is the case that barrier cannot cover - one
// recording writing a buffer in a dispatch and reading it in the next.
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

// Remembers what a buffer is being used as without recording anything, for the
// one caller that cannot record: a render pass, which is inside
// vkCmdBeginRendering by the time it binds anything and where a
// vkCmdPipelineBarrier2 naming a buffer is not merely wasteful but illegal.
//
// What makes it sound is barrierBeforeRendering, which the frame records once
// before the render pass begins and which covers every buffer the pass could
// possibly read against every upload and every dispatch that came before it. So
// the bind has nothing left to order; what it still has to do is leave the
// tracking honest, or the next kernel to *write* this buffer in the same
// recording would barrier from whatever it was before the draw instead of from
// the draw's own read.
inline void noteBufferUse(CommandContext& commands,
                          VulkanBufferData& data,
                          const BufferUse& target)
{
    if (data.buffer == VK_NULL_HANDLE)
        return;

    data.recordingId = commands.recordingId;
    data.use = target;
}

// The one barrier a render pass gets, recorded by the frame just before
// vkCmdBeginRendering because that is the last moment one can be recorded at
// all: Vulkan forbids buffer and image barriers inside a render pass instance,
// so a pass cannot order its own binds and every bind it makes has to be
// covered in advance.
//
// Conservative on purpose, and one global memory barrier rather than one per
// resource for the reason barrierAfterDispatch is: the pass does not yet know
// what it is going to bind. It orders everything the recording has written so
// far - an upload's copy, a kernel's stores, an earlier pass's attachment
// writes - against everything this pass can read or write, which is what makes
// "bind whatever you like, nothing else to do" true for the twenty-four entry
// points of RenderPass.
//
// The attachment stages are in the source mask as well as the destination for
// the case that has no other cover: two passes in a row into the same target
// find its colour image already in COLOR_ATTACHMENT_OPTIMAL, so the layout
// transition that would otherwise have ordered them records nothing.
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

// Orders a dispatch's writes against any later read or write of the same
// memory in this recording - chained kernels, a readback copy, an indirect
// dispatch reading a grid an earlier kernel wrote. One global barrier rather
// than one per resource, which is what D3D12 records here too
// (ComputePass-Windows.cpp) and for the same reason: the pass does not know
// which of the bound buffers the kernel actually wrote.
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
