#include "Frame.h"

#include "../Device/Device.h"
#include "../Texture/MipChain.h"
#include "../Vulkan/VulkanTypes.h"

#include <eacp/Core/Utils/Logging.h>

#include <algorithm>

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

// What a pass does with one attachment and which of its images it draws
// into: ColorTarget's level, slice and layerCount.
struct VulkanClear
{
    Graphics::Color color = Graphics::Color::black();
    bool clear = true;
    int level = 0;
    int slice = 0;
    int layerCount = 1;
};
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
        VulkanTextureData* const targets[] = {&data};
        const VulkanClear clears[] = {{descriptor.clearColor, descriptor.clear}};

        return beginPassOn(targets, clears, descriptor, data, 0, 0);
    }

    // `depthData` is the target whose depth plane is attached, at `depthLevel`
    // and from `depthSlice`: the first colour target's own unless the pass
    // named a DepthTarget.
    RenderPass beginPassOn(std::span<VulkanTextureData* const> targets,
                           std::span<const VulkanClear> clears,
                           const RenderPassDescriptor& descriptor,
                           VulkanTextureData& depthData,
                           int depthLevel,
                           int depthSlice)
    {
        if (commands == nullptr)
            return RenderPass(nullptr);

        for (auto* target: targets)
            if (!target->isValid() || !target->isRenderTarget())
                return RenderPass(nullptr);

        const auto limit =
            getVulkanShared().getProperties().limits.maxColorAttachments;

        if (targets.size() > limit)
        {
            LOG("Vulkan: a pass over ",
                targets.size(),
                " targets is refused; this device offers ",
                limit);
            return RenderPass(nullptr);
        }

        auto& data = *targets[0];
        auto buffer = commands->buffer;

        auto views = std::array<VkImageView, maxColorAttachments> {};

        for (auto index = std::size_t {0}; index < targets.size(); ++index)
        {
            const auto& at = clears[index];
            views[index] = vulkanColorTargetView(
                *targets[index], at.level, at.slice, at.layerCount);

            if (views[index] == VK_NULL_HANDLE)
                return RenderPass(nullptr);
        }

        const auto& first = clears[0];
        const auto depthView = vulkanDepthTargetView(
            depthData, depthLevel, depthSlice, first.layerCount);
        const auto width =
            static_cast<std::uint32_t>(mipExtent(data.width, first.level));
        const auto height =
            static_cast<std::uint32_t>(mipExtent(data.height, first.level));

        // Barriers are illegal once vkCmdBeginRendering has run.
        barrierBeforeRendering(buffer);

        auto colorAttachments =
            std::array<VkRenderingAttachmentInfo, maxColorAttachments> {};

        for (auto index = std::size_t {0}; index < targets.size(); ++index)
        {
            auto& target = *targets[index];

            // COLOR_ATTACHMENT either way: the texture is the resolve
            // destination when the target is multisampled.
            transitionMultisampleForUse(buffer, target, imageColorAttachment);
            transitionTextureForUse(buffer, target, imageColorAttachment);

            const auto& color = clears[index].color;

            auto& colorAttachment = colorAttachments[index];
            colorAttachment.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
            colorAttachment.imageView = views[index];
            colorAttachment.imageLayout = imageColorAttachment.layout;
            colorAttachment.loadOp = clears[index].clear
                                         ? VK_ATTACHMENT_LOAD_OP_CLEAR
                                         : VK_ATTACHMENT_LOAD_OP_LOAD;

            // Stored even when the samples are resolved away: a second pass into
            // the target loads the multisample image, not the resolve.
            colorAttachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
            colorAttachment.clearValue.color = {
                {color.r, color.g, color.b, color.a}};

            if (target.isMultisampled())
            {
                colorAttachment.resolveMode = VK_RESOLVE_MODE_AVERAGE_BIT;
                colorAttachment.resolveImageView = target.colorResolveView();
                colorAttachment.resolveImageLayout = imageColorAttachment.layout;
            }
        }

        transitionDepthForUse(buffer, depthData, imageDepthAttachment);
        transitionResolvedDepthForUse(buffer, depthData, imageDepthAttachment);

        VkRenderingAttachmentInfo depthAttachment = {};
        depthAttachment.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
        depthAttachment.imageView = depthView;
        depthAttachment.imageLayout = imageDepthAttachment.layout;
        depthAttachment.loadOp = vulkanDepthLoadOp(descriptor.depthAction);
        depthAttachment.storeOp = vulkanDepthStoreOp(descriptor.depthAction);
        depthAttachment.clearValue.depthStencil = {
            1.f, static_cast<std::uint32_t>(descriptor.clearStencil)};

        // Where independentResolveNone is false both planes must resolve
        // identically, so the stencil attachment shares this mode and view.
        // Texture refuses such a target where the device cannot resolve it, so
        // a resolve view here always has a mode to be resolved with.
        if (depthData.resolvedDepthAttachmentView != VK_NULL_HANDLE)
        {
            depthAttachment.resolveMode = VK_RESOLVE_MODE_SAMPLE_ZERO_BIT;
            depthAttachment.resolveImageView = depthData.resolvedDepthAttachmentView;
            depthAttachment.resolveImageLayout = imageDepthAttachment.layout;
        }

        VkRenderingInfo rendering = {};
        rendering.sType = VK_STRUCTURE_TYPE_RENDERING_INFO;
        rendering.renderArea.extent = {width, height};
        rendering.layerCount = static_cast<std::uint32_t>(first.layerCount);
        rendering.colorAttachmentCount = static_cast<std::uint32_t>(targets.size());
        rendering.pColorAttachments = colorAttachments.data();

        if (depthView != VK_NULL_HANDLE)
        {
            rendering.pDepthAttachment = &depthAttachment;

            if (depthData.hasStencil())
                rendering.pStencilAttachment = &depthAttachment;
        }

        const auto usesRenderPasses = getVulkanShared().usesRenderPasses();
        auto renderPass = VulkanRenderPassBegin {};

        if (usesRenderPasses)
        {
            renderPass =
                prepareVulkanRenderPass(rendering, targets, depthData.depthFormat);

            if (!renderPass.isValid())
                return RenderPass(nullptr);
        }

        auto* encoder = new VulkanRenderEncoder {
            commands, {}, static_cast<int>(width), static_cast<int>(height)};

        for (auto* target: targets)
            encoder->targets.add(target);

        encoder->depthTarget = &depthData;

        timePass(*encoder, descriptor.label);

        // Before rendering begins, the set's first reset being illegal inside
        // it.
        auto* queries = Frame::occlusionQueriesFor(descriptor);

        if (queries != nullptr)
            queries->notePass(commands);

        if (usesRenderPasses)
            beginVulkanRenderPass(buffer, renderPass);
        else
            vkCmdBeginRendering(buffer, &rendering);

        // A copy inside a render pass instance is illegal, so until
        // RenderPass::end an upload takes a recording of its own.
        context().setRenderPassOpen(true);

        const VkViewport viewport {0.f,
                                   static_cast<float>(height),
                                   static_cast<float>(width),
                                   -static_cast<float>(height),
                                   0.f,
                                   1.f};

        const VkRect2D scissor {{0, 0}, {width, height}};

        vkCmdSetViewport(buffer, 0, 1, &viewport);
        vkCmdSetScissor(buffer, 0, 1, &scissor);

        // Command-buffer state, so it would otherwise carry into the next pass.
        vkCmdSetStencilReference(buffer, VK_STENCIL_FACE_FRONT_AND_BACK, 0);

        return RenderPass(
            encoder, static_cast<int>(width), static_cast<int>(height), queries);
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

Frame::Frame(Device& device,
             void* drawable,
             void* msaaTexture,
             void* depthTexture,
             float backingScaleToUse)
    : nesting(device, *this)
    , impl(device, drawable, msaaTexture, depthTexture)
    , scale(backingScaleToUse)
{
    device.beginFrame();
    impl->beginTiming();
}

Frame::Frame(Device& device, const OffscreenTarget& target, float backingScaleToUse)
    : nesting(device, *this)
    , impl(device, target)
    , scale(backingScaleToUse)
{
    device.beginFrame();
    impl->beginTiming();
}

// The swapchain image for a drawable frame and the app's texture off screen are
// the same VulkanTextureData either way - the one beginPassOn takes its render
// area and viewport from.
Graphics::Point Frame::nativePixelSize() const
{
    if (impl->target == nullptr)
        return {};

    return {static_cast<float>(impl->target->width),
            static_cast<float>(impl->target->height)};
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

RenderPass Frame::beginNativePass(const RenderPassDescriptor& descriptor)
{
    if (impl->target == nullptr)
        return RenderPass(nullptr);

    return impl->beginPassOn(*impl->target, descriptor);
}

RenderPass Frame::beginMultiTargetPass(std::span<const ColorTarget> targets,
                                       const RenderPassDescriptor& descriptor)
{
    if (!canRenderInto(targets, descriptor))
        return RenderPass(nullptr);

    auto data = std::array<VulkanTextureData*, maxColorAttachments> {};
    auto clears = std::array<VulkanClear, maxColorAttachments> {};

    for (auto index = std::size_t {0}; index < targets.size(); ++index)
    {
        data[index] =
            static_cast<VulkanTextureData*>(targets[index].texture->nativeTexture());
        const auto& target = targets[index];
        clears[index] = {target.clearColor,
                         target.clear,
                         target.level,
                         target.slice,
                         target.layerCount};

        if (data[index] == nullptr)
            return RenderPass(nullptr);
    }

    const auto depthFrom = depthSource(targets, descriptor);
    auto* depthData =
        static_cast<VulkanTextureData*>(depthFrom.texture->nativeTexture());

    if (depthData == nullptr)
        return RenderPass(nullptr);

    return impl->beginPassOn(std::span {data.data(), targets.size()},
                             std::span {clears.data(), targets.size()},
                             descriptor,
                             *depthData,
                             depthFrom.level,
                             depthFrom.slice);
}

ComputePass Frame::beginCompute(std::string_view label, DispatchOrder order)
{
    if (impl->commands == nullptr)
        return ComputePass(nullptr, order);

    auto* encoder = new VulkanComputeEncoder {impl->commands};
    impl->timePass(*encoder, label);

    return ComputePass(encoder, order);
}

TextureFormat Frame::nativeDrawableFormat() const
{
    if (impl->target == nullptr)
        return TextureFormat::BGRA8Unorm;

    switch (impl->target->format)
    {
        case VK_FORMAT_R8G8B8A8_UNORM:
            return TextureFormat::RGBA8Unorm;
        case VK_FORMAT_R8G8B8A8_SRGB:
            return TextureFormat::RGBA8UnormSrgb;
        case VK_FORMAT_B8G8R8A8_SRGB:
            return TextureFormat::BGRA8UnormSrgb;
        case VK_FORMAT_R16G16B16A16_SFLOAT:
            return TextureFormat::RGBA16Float;
        default:
            return TextureFormat::BGRA8Unorm;
    }
}

// A vkCmdCopyImage on the frame's recording, outside any pass. A multisampled
// view's pass resolves into the swapchain image as it ends, so the image holds
// the picture either way. The image is left where the copy put it; the next
// pass or the present moves it from there.
bool Frame::copyNativeDrawable(const Texture& destination)
{
    if (impl->commands == nullptr || impl->target == nullptr
        || !canCopyDrawableInto(destination))
        return false;

    if (impl->drawable != nullptr && !impl->drawable->readable)
    {
        LOG("Vulkan: copyDrawable on a view whose swapchain is not a transfer "
            "source; see GPUView::setReadableDrawable");
        return false;
    }

    if (impl->context().isRenderPassOpen())
    {
        LOG("eacp: copyDrawable while a pass is open; end the pass first");
        return false;
    }

    auto* data = static_cast<VulkanTextureData*>(destination.nativeTexture());

    if (data == nullptr || data->image == VK_NULL_HANDLE)
        return false;

    auto buffer = impl->commands->buffer;
    auto& source = *impl->target;

    transitionTextureForUse(buffer, source, imageTransferSrc);
    transitionTextureForUse(buffer, *data, imageTransferDst);

    VkImageCopy region = {};
    region.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    region.srcSubresource.layerCount = 1;
    region.dstSubresource = region.srcSubresource;
    region.extent = {static_cast<std::uint32_t>(source.width),
                     static_cast<std::uint32_t>(source.height),
                     1};

    vkCmdCopyImage(buffer,
                   source.image,
                   imageTransferSrc.layout,
                   data->image,
                   imageTransferDst.layout,
                   1,
                   &region);

    transitionTextureForUse(buffer, *data, data->restingUse());
    return true;
}

bool Frame::isNativeValid() const
{
    return impl->commands != nullptr && impl->target != nullptr
           && impl->target->isValid();
}

void Frame::resumeRecording()
{
    if (impl->commands != nullptr)
        impl->context().setOpenRecording(impl->commands);
}

void* Frame::nativeRecording() const
{
    return impl->commands;
}
} // namespace eacp::GPU
