#include "Frame.h"

#include "../Device/Device.h"
#include "../Vulkan/VulkanContext.h"
#include "../Vulkan/VulkanTypes.h"

namespace eacp::GPU
{
// Defined in Texture-Vulkan.cpp.
void transitionImage(VkCommandBuffer list,
                     VulkanTexture& texture,
                     VkImageLayout target);

struct Frame::Native
{
    Native(VulkanDrawable* drawableToUse,
           VulkanTexture* colorToUse,
           VulkanTexture* msaaToUse,
           VulkanTexture* depthToUse)
        : drawable(drawableToUse)
        , color(colorToUse)
        , msaa(msaaToUse)
        , depth(depthToUse)
    {
        auto& context = getVulkanContext();

        if (!context.isValid() || color == nullptr)
            return;

        commands = context.acquire();

        if (commands == nullptr)
            return;

        renderTarget.list = commands->list;
        renderTarget.commands = commands;
        renderTarget.width = color->width;
        renderTarget.height = color->height;

        transitionImage(
            commands->list, *color, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);

        if (msaa != nullptr)
            transitionImage(
                commands->list, *msaa, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    }

    ~Native()
    {
        auto& context = getVulkanContext();

        if (commands == nullptr || !context.isValid())
            return;

        if (drawable == nullptr)
        {
            // Off-screen frames never present: they leave the colour image ready
            // to be copied out and block until the GPU is done, so the caller can
            // read the pixels back straight away.
            transitionImage(
                commands->list, *color, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
            context.waitFor(context.submit(commands));
            return;
        }

        // On-screen: hand the image to the presentation engine, and gate the
        // present on the render finishing rather than blocking the CPU on it.
        transitionImage(commands->list, *color, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);
        context.submit(commands, drawable->acquired, drawable->rendered);

        auto info = VkPresentInfoKHR {.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
        info.waitSemaphoreCount = 1;
        info.pWaitSemaphores = &drawable->rendered;
        info.swapchainCount = 1;
        info.pSwapchains = &drawable->swapchain;
        info.pImageIndices = &drawable->imageIndex;

        vkQueuePresentKHR(context.getQueue(), &info);
    }

    VulkanDrawable* drawable = nullptr;
    VulkanTexture* color = nullptr;
    VulkanTexture* msaa = nullptr;
    VulkanTexture* depth = nullptr;
    CommandContext* commands = nullptr;
    VulkanRenderTarget renderTarget;
};

Frame::Frame(Device&, void* drawable, void* msaaTexture, void* depthTexture)
    : impl(static_cast<VulkanDrawable*>(drawable),
           drawable != nullptr ? static_cast<VulkanDrawable*>(drawable)->image
                               : nullptr,
           static_cast<VulkanTexture*>(msaaTexture),
           static_cast<VulkanTexture*>(depthTexture))
{
}

Frame::Frame(Device&, const OffscreenTarget& target)
    : impl(nullptr,
           static_cast<VulkanTexture*>(target.colorTexture),
           static_cast<VulkanTexture*>(target.msaaTexture),
           static_cast<VulkanTexture*>(target.depthTexture))
{
}

Frame::~Frame() = default;

RenderPass Frame::beginPass(const RenderPassDescriptor& descriptor)
{
    if (!isValid())
        return RenderPass {nullptr};

    auto* resolveInto = impl->msaa != nullptr ? impl->color : nullptr;
    auto* attachment = impl->msaa != nullptr ? impl->msaa : impl->color;

    auto color = VkRenderingAttachmentInfoKHR {
        .sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO_KHR};
    color.imageView = attachment->view;
    color.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    color.loadOp =
        descriptor.clear ? VK_ATTACHMENT_LOAD_OP_CLEAR : VK_ATTACHMENT_LOAD_OP_LOAD;
    color.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    color.clearValue.color = {{descriptor.clearColor.r,
                               descriptor.clearColor.g,
                               descriptor.clearColor.b,
                               descriptor.clearColor.a}};

    if (resolveInto != nullptr)
    {
        color.resolveMode = VK_RESOLVE_MODE_AVERAGE_BIT;
        color.resolveImageView = resolveInto->view;
        color.resolveImageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    }

    auto depth = VkRenderingAttachmentInfoKHR {
        .sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO_KHR};

    if (impl->depth != nullptr)
    {
        depth.imageView = impl->depth->view;
        depth.imageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
        depth.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        depth.storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        depth.clearValue.depthStencil = {1.f, 0};
    }

    auto info = VkRenderingInfoKHR {.sType = VK_STRUCTURE_TYPE_RENDERING_INFO_KHR};
    info.renderArea.extent = {static_cast<std::uint32_t>(impl->renderTarget.width),
                              static_cast<std::uint32_t>(impl->renderTarget.height)};
    info.layerCount = 1;
    info.colorAttachmentCount = 1;
    info.pColorAttachments = &color;

    if (impl->depth != nullptr)
        info.pDepthAttachment = &depth;

    getVulkanContext().beginRendering(impl->commands->list, info);

    return RenderPass {
        &impl->renderTarget, impl->renderTarget.width, impl->renderTarget.height};
}

bool Frame::isValid() const
{
    return impl->commands != nullptr && impl->color != nullptr;
}
} // namespace eacp::GPU
