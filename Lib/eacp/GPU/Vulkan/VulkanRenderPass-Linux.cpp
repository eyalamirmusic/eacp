#include "../Common.h"

#include "VulkanContext.h"
#include "VulkanTypes.h"

namespace eacp::GPU
{
namespace
{
VkAttachmentDescription2 makeAttachment(VkFormat format,
                                        VkSampleCountFlagBits samples,
                                        VkAttachmentLoadOp load,
                                        VkAttachmentStoreOp store,
                                        VkImageLayout layout)
{
    VkAttachmentDescription2 attachment = {};
    attachment.sType = VK_STRUCTURE_TYPE_ATTACHMENT_DESCRIPTION_2;
    attachment.format = format;
    attachment.samples = samples;
    attachment.loadOp = load;
    attachment.storeOp = store;
    attachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    attachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    attachment.initialLayout = layout;
    attachment.finalLayout = layout;

    return attachment;
}

VkAttachmentReference2 makeReference(std::uint32_t index,
                                     VkImageLayout layout,
                                     VkImageAspectFlags aspect)
{
    VkAttachmentReference2 reference = {};
    reference.sType = VK_STRUCTURE_TYPE_ATTACHMENT_REFERENCE_2;
    reference.attachment = index;
    reference.layout = layout;
    reference.aspectMask = aspect;

    return reference;
}

// Every layout is the one the dynamic path renders in, before and after: the
// barriers around the pass move the images, so the render pass moves nothing and
// its implicit external dependencies order nothing the barriers do not.
VkRenderPass makeRenderPass(VkDevice device, const VulkanRenderPassKey& key)
{
    const auto colorLayout = imageColorAttachment.layout;
    const auto depthLayout = imageDepthAttachment.layout;
    const auto depthAspect = depthAspectMask(key.stencil);

    VkAttachmentDescription2 attachments[4] = {};
    auto count = std::uint32_t {0};

    const auto colorIndex = count;
    attachments[count++] = makeAttachment(
        key.colorFormat, key.samples, key.colorLoad, key.colorStore, colorLayout);

    auto resolveIndex = VK_ATTACHMENT_UNUSED;

    if (key.colorResolve)
    {
        resolveIndex = count;
        attachments[count++] = makeAttachment(key.colorFormat,
                                              VK_SAMPLE_COUNT_1_BIT,
                                              VK_ATTACHMENT_LOAD_OP_DONT_CARE,
                                              VK_ATTACHMENT_STORE_OP_STORE,
                                              colorLayout);
    }

    const auto hasDepth = key.depthFormat != VK_FORMAT_UNDEFINED;
    auto depthIndex = VK_ATTACHMENT_UNUSED;
    auto depthResolveIndex = VK_ATTACHMENT_UNUSED;

    if (hasDepth)
    {
        depthIndex = count;
        auto& depth = attachments[count++];
        depth = makeAttachment(key.depthFormat,
                               key.samples,
                               key.depthLoad,
                               key.depthStore,
                               depthLayout);

        // The dynamic path hands the one attachment as depth and as stencil.
        if (key.stencil)
        {
            depth.stencilLoadOp = key.depthLoad;
            depth.stencilStoreOp = key.depthStore;
        }

        if (key.depthResolve)
        {
            depthResolveIndex = count;
            auto& resolve = attachments[count++];
            resolve = makeAttachment(key.depthFormat,
                                     VK_SAMPLE_COUNT_1_BIT,
                                     VK_ATTACHMENT_LOAD_OP_DONT_CARE,
                                     VK_ATTACHMENT_STORE_OP_STORE,
                                     depthLayout);

            if (key.stencil)
                resolve.stencilStoreOp = VK_ATTACHMENT_STORE_OP_STORE;
        }
    }

    const auto colorReference =
        makeReference(colorIndex, colorLayout, VK_IMAGE_ASPECT_COLOR_BIT);
    const auto resolveReference =
        makeReference(resolveIndex, colorLayout, VK_IMAGE_ASPECT_COLOR_BIT);
    const auto depthReference = makeReference(depthIndex, depthLayout, depthAspect);
    const auto depthResolveReference =
        makeReference(depthResolveIndex, depthLayout, depthAspect);

    VkSubpassDescriptionDepthStencilResolve depthResolve = {};
    depthResolve.sType = VK_STRUCTURE_TYPE_SUBPASS_DESCRIPTION_DEPTH_STENCIL_RESOLVE;
    depthResolve.depthResolveMode = VK_RESOLVE_MODE_SAMPLE_ZERO_BIT;
    depthResolve.stencilResolveMode =
        key.stencil ? VK_RESOLVE_MODE_SAMPLE_ZERO_BIT : VK_RESOLVE_MODE_NONE;
    depthResolve.pDepthStencilResolveAttachment = &depthResolveReference;

    VkSubpassDescription2 subpass = {};
    subpass.sType = VK_STRUCTURE_TYPE_SUBPASS_DESCRIPTION_2;
    subpass.pNext = key.depthResolve ? &depthResolve : nullptr;
    subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.colorAttachmentCount = 1;
    subpass.pColorAttachments = &colorReference;
    subpass.pResolveAttachments = key.colorResolve ? &resolveReference : nullptr;
    subpass.pDepthStencilAttachment = hasDepth ? &depthReference : nullptr;

    VkRenderPassCreateInfo2 info = {};
    info.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO_2;
    info.attachmentCount = count;
    info.pAttachments = attachments;
    info.subpassCount = 1;
    info.pSubpasses = &subpass;

    auto renderPass = VkRenderPass {VK_NULL_HANDLE};

    if (vkCreateRenderPass2(device, &info, nullptr, &renderPass) != VK_SUCCESS)
        return VK_NULL_HANDLE;

    return renderPass;
}
} // namespace

VkRenderPass VulkanRenderPassCache::get(VkDevice device,
                                        const VulkanRenderPassKey& key)
{
    auto lock = std::lock_guard<std::mutex> {mutex};

    for (const auto& entry: renderPasses)
        if (entry.key == key)
            return entry.renderPass;

    const auto renderPass = makeRenderPass(device, key);

    if (renderPass != VK_NULL_HANDLE)
        renderPasses.add({key, renderPass});

    return renderPass;
}

VkFramebuffer VulkanRenderPassCache::getFramebuffer(VkDevice device,
                                                    VkRenderPass renderPass,
                                                    const Vector<VkImageView>& views,
                                                    std::uint32_t width,
                                                    std::uint32_t height)
{
    auto lock = std::lock_guard<std::mutex> {mutex};

    for (const auto& entry: framebuffers)
        if (entry.renderPass == renderPass && entry.width == width
            && entry.height == height && entry.views == views)
            return entry.framebuffer;

    VkFramebufferCreateInfo info = {};
    info.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
    info.renderPass = renderPass;
    info.attachmentCount = static_cast<std::uint32_t>(views.size());
    info.pAttachments = views.data();
    info.width = width;
    info.height = height;
    info.layers = 1;

    auto framebuffer = VkFramebuffer {VK_NULL_HANDLE};

    if (vkCreateFramebuffer(device, &info, nullptr, &framebuffer) != VK_SUCCESS)
        return VK_NULL_HANDLE;

    framebuffers.add({renderPass, views, width, height, framebuffer});
    return framebuffer;
}

void VulkanRenderPassCache::forgetView(VkDevice device, VkImageView view)
{
    auto lock = std::lock_guard<std::mutex> {mutex};

    auto kept = Vector<FramebufferEntry> {};

    for (auto& entry: framebuffers)
    {
        if (entry.views.contains(view))
            vkDestroyFramebuffer(device, entry.framebuffer, nullptr);
        else
            kept.add(std::move(entry));
    }

    framebuffers = std::move(kept);
}

void VulkanRenderPassCache::destroyAll(VkDevice device)
{
    auto lock = std::lock_guard<std::mutex> {mutex};

    for (const auto& entry: framebuffers)
        vkDestroyFramebuffer(device, entry.framebuffer, nullptr);

    for (const auto& entry: renderPasses)
        vkDestroyRenderPass(device, entry.renderPass, nullptr);

    framebuffers.clear();
    renderPasses.clear();
}

// Load and store ops and resolves do not decide compatibility for a render pass
// of one subpass, so one pass per set of formats serves every pipeline.
VkRenderPass VulkanShared::compatibleRenderPass(VkFormat colorFormat,
                                                int samples,
                                                VkFormat depthFormat,
                                                bool stencil)
{
    auto key = VulkanRenderPassKey {};
    key.colorFormat = colorFormat;
    key.samples = toVkSampleCount(samples);
    key.colorResolve = samples > 1;
    key.depthFormat = depthFormat;
    key.stencil = stencil;

    if (depthFormat != VK_FORMAT_UNDEFINED)
    {
        key.depthLoad = VK_ATTACHMENT_LOAD_OP_LOAD;
        key.depthStore = VK_ATTACHMENT_STORE_OP_STORE;
    }

    return renderPasses.get(device, key);
}

void VulkanShared::destroyImageView(VkImageView view)
{
    if (view == VK_NULL_HANDLE || device == VK_NULL_HANDLE)
        return;

    if (renderPassPath)
        renderPasses.forgetView(device, view);

    vkDestroyImageView(device, view, nullptr);
}

VulkanRenderPassBegin prepareVulkanRenderPass(const VkRenderingInfo& rendering,
                                              const VulkanTextureData& data)
{
    auto& shared = getVulkanShared();

    const auto& color = rendering.pColorAttachments[0];
    const auto* depth = rendering.pDepthAttachment;

    auto key = VulkanRenderPassKey {};
    key.colorFormat = data.format;
    key.samples = toVkSampleCount(data.isMultisampled() ? data.sampleCount : 1);
    key.colorLoad = color.loadOp;
    key.colorStore = color.storeOp;
    key.colorResolve = color.resolveImageView != VK_NULL_HANDLE;

    auto begin = VulkanRenderPassBegin {};
    begin.renderArea = rendering.renderArea;

    auto views = Vector<VkImageView> {};

    views.add(color.imageView);
    begin.clearValues.add(color.clearValue);

    if (key.colorResolve)
    {
        views.add(color.resolveImageView);
        begin.clearValues.add(VkClearValue {});
    }

    if (depth != nullptr)
    {
        key.depthFormat = data.depthFormat;
        key.stencil = rendering.pStencilAttachment != nullptr;
        key.depthLoad = depth->loadOp;
        key.depthStore = depth->storeOp;
        key.depthResolve = depth->resolveImageView != VK_NULL_HANDLE;

        views.add(depth->imageView);
        begin.clearValues.add(depth->clearValue);

        if (key.depthResolve)
        {
            views.add(depth->resolveImageView);
            begin.clearValues.add(VkClearValue {});
        }
    }

    begin.renderPass = shared.getRenderPasses().get(shared.getDevice(), key);

    if (begin.renderPass == VK_NULL_HANDLE)
        return {};

    begin.framebuffer =
        shared.getRenderPasses().getFramebuffer(shared.getDevice(),
                                                begin.renderPass,
                                                views,
                                                rendering.renderArea.extent.width,
                                                rendering.renderArea.extent.height);

    return begin;
}

void beginVulkanRenderPass(VkCommandBuffer commandBuffer,
                           const VulkanRenderPassBegin& begin)
{
    VkRenderPassBeginInfo info = {};
    info.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    info.renderPass = begin.renderPass;
    info.framebuffer = begin.framebuffer;
    info.renderArea = begin.renderArea;
    info.clearValueCount = static_cast<std::uint32_t>(begin.clearValues.size());
    info.pClearValues = begin.clearValues.data();

    vkCmdBeginRenderPass(commandBuffer, &info, VK_SUBPASS_CONTENTS_INLINE);
}
} // namespace eacp::GPU
