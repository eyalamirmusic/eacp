#include "Frame.h"

#include "../Device/Device.h"
#include "../Vulkan/VulkanBackend-Linux.h"
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

struct VulkanFrameBackend final : FrameBackend
{
    VulkanFrameBackend(Device& deviceToUse, void* drawablePointer)
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

    VulkanFrameBackend(Device& deviceToUse,
                       const OffscreenTarget& offscreenTarget)
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
    void beginTiming() override
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

    std::unique_ptr<RenderPassBackend>
        beginPassOn(VulkanTextureData& data, const RenderPassDescriptor& descriptor)
    {
        if (commands == nullptr || !data.isValid() || !data.isRenderTarget())
            return nullptr;

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

        return makeVulkanRenderPass(encoder, data.width, data.height);
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
    // The frame's work is submitted when the backend goes, which is when the
    // Frame above it does: everything recorded on it is ended, presented where
    // there is a drawable and waited for where there is not.
    ~VulkanFrameBackend() override
    {
        close();

        if (commands == nullptr)
            return;

        auto& commandContext = context();

        // vkQueuePresentKHR requires PRESENT_SRC_KHR, and a frame that opened no
        // pass never moved the image out of UNDEFINED.
        if (drawable != nullptr && target != nullptr)
            transitionTextureForUse(commands->buffer, *target, imagePresent);

        device->frameTimer().endFrame(commands->buffer);

        auto sync = SubmitSync {};

        if (drawable != nullptr)
        {
            sync.wait = pendingWait;
            sync.signal = drawable->renderFinished;
            pendingWait = VK_NULL_HANDLE;
        }

        const auto submitted = commandContext.submit(commands, sync);
        device->frameTimer().noteSubmitted(submitted);

        // Texture::read is only valid once the render has finished.
        if (drawable != nullptr)
            present(submitted);
        else if (offscreen)
            commandContext.waitIdle();
    }

    bool isValid() const override
    {
        return commands != nullptr && target != nullptr && target->isValid();
    }

    // The swapchain image for a drawable frame and the app's texture off screen
    // are the same VulkanTextureData either way - the one beginPassOn takes its
    // render area and viewport from.
    Graphics::Point pixelSize() const override
    {
        if (target == nullptr)
            return {};

        return {static_cast<float>(target->width),
                static_cast<float>(target->height)};
    }

    void flush() override
    {
        if (commands == nullptr)
            return;

        auto& commandContext = context();

        // Before the submit: an upload must never be handed a buffer about to
        // end.
        close();

        // The acquire goes on this submission; the render-finished semaphore
        // belongs to the frame's last one, which this is not.
        auto sync = SubmitSync {};
        sync.wait = pendingWait;
        pendingWait = VK_NULL_HANDLE;

        commandContext.submit(commands, sync);

        open(commandContext.acquire());
    }

    std::unique_ptr<RenderPassBackend>
        beginPass(const RenderPassDescriptor& descriptor) override
    {
        if (target == nullptr)
            return nullptr;

        return beginPassOn(*target, descriptor);
    }

    std::unique_ptr<RenderPassBackend>
        beginPass(const Texture& passTarget,
                  const RenderPassDescriptor& descriptor) override
    {
        auto* data = static_cast<VulkanTextureData*>(passTarget.nativeTexture());

        if (data == nullptr || !passTarget.isRenderTarget())
            return nullptr;

        return beginPassOn(*data, descriptor);
    }

    std::unique_ptr<ComputePassBackend> beginCompute(std::string_view label,
                                                     DispatchOrder order) override
    {
        if (commands == nullptr)
            return nullptr;

        auto* encoder = new VulkanComputeEncoder {commands};
        timePass(*encoder, label);

        return makeVulkanComputePass(encoder, order);
    }
};
} // namespace

std::unique_ptr<FrameBackend> makeVulkanFrame(Device& device, void* drawable)
{
    return std::make_unique<VulkanFrameBackend>(device, drawable);
}

std::unique_ptr<FrameBackend> makeVulkanFrame(Device& device,
                                              const OffscreenTarget& target)
{
    return std::make_unique<VulkanFrameBackend>(device, target);
}
} // namespace eacp::GPU
