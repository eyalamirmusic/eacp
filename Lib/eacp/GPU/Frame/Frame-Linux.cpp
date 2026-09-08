#include "Frame.h"

#include "../Device/Device.h"
#include "../Vulkan/VulkanTypes.h"

namespace eacp::GPU
{
namespace
{
VkAttachmentLoadOp vulkanDepthLoadOp(DepthAction action)
{
    return action == DepthAction::Resume ? VK_ATTACHMENT_LOAD_OP_LOAD
                                         : VK_ATTACHMENT_LOAD_OP_CLEAR;
}

VkAttachmentStoreOp vulkanDepthStoreOp(DepthAction action)
{
    return action == DepthAction::Clear ? VK_ATTACHMENT_STORE_OP_DONT_CARE
                                        : VK_ATTACHMENT_STORE_OP_STORE;
}
} // namespace

struct Frame::Native
{
    Native(Device& deviceToUse, void* drawablePointer, void*, void*)
        : device(&deviceToUse)
        , drawable(static_cast<VulkanDrawable*>(drawablePointer))
    {
        if (drawable == nullptr || drawable->target == nullptr)
            return;

        target = drawable->target;

        pendingWait = drawable->acquired;

        if (deviceToUse.isValid() && target->isValid() && target->isRenderTarget())
            open(context().acquire());
    }

    Native(Device& deviceToUse, const OffscreenTarget& offscreenTarget)
        : device(&deviceToUse)
        , target(static_cast<VulkanTextureData*>(offscreenTarget.colorTexture))
        , offscreen(true)
    {
        if (deviceToUse.isValid() && target != nullptr && target->isValid()
            && target->isRenderTarget())
            open(context().acquire());
    }

    void open(CommandContext* commandsToUse)
    {
        commands = commandsToUse;

        if (commands != nullptr)
            context().setOpenRecording(commands);
    }

    void close()
    {
        if (commands != nullptr && context().getOpenRecording() == commands)
            context().setOpenRecording(nullptr);

        context().setRenderPassOpen(false);
    }

    VulkanContext& context() const { return getVulkanContext(*device); }

    VkCommandBuffer commandBuffer() const
    {
        return commands != nullptr ? commands->buffer : VK_NULL_HANDLE;
    }

    // Has to be the first thing on the command buffer for the total to mean the
    // frame, and to run after Device::beginFrame() has given the timer its slot.
    void beginTiming()
    {
        if (commands != nullptr)
            device->frameTimer().beginRecording(commands->buffer);
    }

    template <typename Encoder>
    void timePass(Encoder& encoder, std::string_view label)
    {
        auto& timer = device->frameTimer();
        const auto pass = timer.beginPass(label);

        if (pass < 0 || commands == nullptr)
            return;

        auto queryPool = static_cast<VkQueryPool>(timer.nativeSamples());

        if (queryPool == VK_NULL_HANDLE)
            return;

        vkCmdWriteTimestamp2(commands->buffer,
                             VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT,
                             queryPool,
                             static_cast<std::uint32_t>(pass * 2));

        encoder.queryPool = queryPool;
        encoder.endQuery = pass * 2 + 1;
    }

    RenderPass beginPassOn(VulkanTextureData& data,
                           const RenderPassDescriptor& descriptor)
    {
        if (commands == nullptr || !data.isValid() || !data.isRenderTarget())
            return RenderPass(nullptr);

        auto buffer = commands->buffer;

        // Barriers are illegal once vkCmdBeginRendering has run.
        barrierBeforeRendering(buffer);

        // COLOR_ATTACHMENT either way: the texture is the resolve destination
        // when the target is multisampled.
        transitionMultisampleForUse(buffer, data, imageColorAttachment);
        transitionTextureForUse(buffer, data, imageColorAttachment);
        transitionDepthForUse(buffer, data, imageDepthAttachment);
        transitionResolvedDepthForUse(buffer, data, imageDepthAttachment);

        const auto& color = descriptor.clearColor;

        VkRenderingAttachmentInfo colorAttachment = {};
        colorAttachment.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
        colorAttachment.imageView = data.colorAttachmentView();
        colorAttachment.imageLayout = imageColorAttachment.layout;
        colorAttachment.loadOp = descriptor.clear ? VK_ATTACHMENT_LOAD_OP_CLEAR
                                                  : VK_ATTACHMENT_LOAD_OP_LOAD;

        // Stored even when the samples are resolved away: a second pass into
        // the target loads the multisample image, not the resolve.
        colorAttachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        colorAttachment.clearValue.color = {{color.r, color.g, color.b, color.a}};

        if (data.isMultisampled())
        {
            colorAttachment.resolveMode = VK_RESOLVE_MODE_AVERAGE_BIT;
            colorAttachment.resolveImageView = data.colorResolveView();
            colorAttachment.resolveImageLayout = imageColorAttachment.layout;
        }

        VkRenderingAttachmentInfo depthAttachment = {};
        depthAttachment.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
        depthAttachment.imageView = data.depthAttachmentView;
        depthAttachment.imageLayout = imageDepthAttachment.layout;
        depthAttachment.loadOp = vulkanDepthLoadOp(descriptor.depthAction);
        depthAttachment.storeOp = vulkanDepthStoreOp(descriptor.depthAction);
        depthAttachment.clearValue.depthStencil = {
            1.f, static_cast<std::uint32_t>(descriptor.clearStencil)};

        // Where independentResolveNone is false both planes must resolve
        // identically, so the stencil attachment shares this mode and view.
        // Texture refuses such a target where the device cannot resolve it, so
        // a resolve view here always has a mode to be resolved with.
        if (data.resolvedDepthAttachmentView != VK_NULL_HANDLE)
        {
            depthAttachment.resolveMode = VK_RESOLVE_MODE_SAMPLE_ZERO_BIT;
            depthAttachment.resolveImageView = data.resolvedDepthAttachmentView;
            depthAttachment.resolveImageLayout = imageDepthAttachment.layout;
        }

        VkRenderingInfo rendering = {};
        rendering.sType = VK_STRUCTURE_TYPE_RENDERING_INFO;
        rendering.renderArea.extent = {static_cast<std::uint32_t>(data.width),
                                       static_cast<std::uint32_t>(data.height)};
        rendering.layerCount = 1;
        rendering.colorAttachmentCount = 1;
        rendering.pColorAttachments = &colorAttachment;

        if (data.hasDepth())
        {
            rendering.pDepthAttachment = &depthAttachment;

            if (data.hasStencil())
                rendering.pStencilAttachment = &depthAttachment;
        }

        auto* encoder =
            new VulkanRenderEncoder {commands, &data, data.width, data.height};
        timePass(*encoder, descriptor.label);

        vkCmdBeginRendering(buffer, &rendering);

        // A copy inside a render pass instance is illegal, so until
        // RenderPass::end an upload takes a recording of its own.
        context().setRenderPassOpen(true);

        const VkViewport viewport {0.f,
                                   static_cast<float>(data.height),
                                   static_cast<float>(data.width),
                                   -static_cast<float>(data.height),
                                   0.f,
                                   1.f};

        const VkRect2D scissor {{0, 0},
                                {static_cast<std::uint32_t>(data.width),
                                 static_cast<std::uint32_t>(data.height)}};

        vkCmdSetViewport(buffer, 0, 1, &viewport);
        vkCmdSetScissor(buffer, 0, 1, &scissor);

        // Command-buffer state, so it would otherwise carry into the next pass.
        vkCmdSetStencilReference(buffer, VK_STENCIL_FACE_FRONT_AND_BACK, 0);

        return RenderPass(encoder, data.width, data.height);
    }

    // A VkQueue is externally synchronized: presenting takes the submit mutex.
    void present(std::uint64_t submittedValue)
    {
        // A refused submission signals nothing, and presenting on a semaphore
        // that never signals hangs the surface rather than dropping a frame.
        if (submittedValue == 0)
            return;

        VkPresentInfoKHR info = {};
        info.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
        info.waitSemaphoreCount = 1;
        info.pWaitSemaphores = &drawable->renderFinished;
        info.swapchainCount = 1;
        info.pSwapchains = &drawable->swapchain;
        info.pImageIndices = &drawable->imageIndex;

        auto lock = std::lock_guard<std::mutex> {getVulkanShared().getQueueMutex()};

        drawable->presentResult = vkQueuePresentKHR(context().getQueue(), &info);
    }

    Device* device = nullptr;
    CommandContext* commands = nullptr;

    VulkanTextureData* target = nullptr;

    VulkanDrawable* drawable = nullptr;

    VkSemaphore pendingWait = VK_NULL_HANDLE;

    bool offscreen = false;
};

Frame::Frame(Device& device, void* drawable, void* msaaTexture, void* depthTexture)
    : impl(device, drawable, msaaTexture, depthTexture)
{
    device.beginFrame();
    impl->beginTiming();
}

Frame::Frame(Device& device, const OffscreenTarget& target)
    : impl(device, target)
{
    device.beginFrame();
    impl->beginTiming();
}

Frame::~Frame()
{
    impl->close();

    if (impl->commands == nullptr)
        return;

    auto& context = impl->context();

    // vkQueuePresentKHR requires PRESENT_SRC_KHR, and a frame that opened no
    // pass never moved the image out of UNDEFINED.
    if (impl->drawable != nullptr && impl->target != nullptr)
        transitionTextureForUse(impl->commands->buffer, *impl->target, imagePresent);

    impl->device->frameTimer().endFrame(impl->commands->buffer);

    auto sync = SubmitSync {};

    if (impl->drawable != nullptr)
    {
        sync.wait = impl->pendingWait;
        sync.signal = impl->drawable->renderFinished;
        impl->pendingWait = VK_NULL_HANDLE;
    }

    const auto submitted = context.submit(impl->commands, sync);
    impl->device->frameTimer().noteSubmitted(submitted);

    // Texture::read is only valid once the render has finished.
    if (impl->drawable != nullptr)
        impl->present(submitted);
    else if (impl->offscreen)
        context.waitIdle();
}

void Frame::flush()
{
    if (impl->commands == nullptr)
        return;

    auto& context = impl->context();

    // Before the submit: an upload must never be handed a buffer about to end.
    impl->close();

    // The acquire goes on this submission; the render-finished semaphore
    // belongs to the frame's last one, which this is not.
    auto sync = SubmitSync {};
    sync.wait = impl->pendingWait;
    impl->pendingWait = VK_NULL_HANDLE;

    context.submit(impl->commands, sync);

    impl->open(context.acquire());
}

RenderPass Frame::beginPass(const RenderPassDescriptor& descriptor)
{
    if (impl->target == nullptr)
        return RenderPass(nullptr);

    return impl->beginPassOn(*impl->target, descriptor);
}

RenderPass Frame::beginPass(const Texture& target,
                            const RenderPassDescriptor& descriptor)
{
    auto* data = static_cast<VulkanTextureData*>(target.nativeTexture());

    if (data == nullptr || !target.isRenderTarget())
        return RenderPass(nullptr);

    return impl->beginPassOn(*data, descriptor);
}

ComputePass Frame::beginCompute(std::string_view label, DispatchOrder order)
{
    if (impl->commands == nullptr)
        return ComputePass(nullptr, order);

    auto* encoder = new VulkanComputeEncoder {impl->commands};
    impl->timePass(*encoder, label);

    return ComputePass(encoder, order);
}

bool Frame::isValid() const
{
    return impl->commands != nullptr && impl->target != nullptr
           && impl->target->isValid();
}
} // namespace eacp::GPU
