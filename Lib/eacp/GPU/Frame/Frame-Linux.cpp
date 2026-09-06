#include "Frame.h"

#include "../Device/Device.h"
#include "../Vulkan/VulkanTypes.h"

// Linux/Vulkan backend. The frame owns one CommandContext recording for its
// lifetime: every pass records onto it, flush() submits it and takes another,
// and the destructor submits the last one.
//
// **Dynamic rendering throughout**, so there is no VkRenderPass and no
// VkFramebuffer: a pass is a vkCmdBeginRendering naming the views it draws into
// and the load and store each of them takes. What that costs is the rule the
// rest of this file is arranged around - barriers cannot be recorded inside a
// render pass instance, so every image a pass touches has to be moved into
// place *before* vkCmdBeginRendering and moved back after vkCmdEndRendering,
// and every buffer the pass may read has to have been made visible in advance
// by the one global barrier barrierBeforeRendering records.
//
// **Both constructors take the same shape of target**, which is the one thing
// this backend does differently from the D3D12 one. Off-screen, the colour
// target is a real GPU::Texture that GPUView created with renderTarget; on the
// drawable path it is a swapchain image GPUView described the same way. Either
// way the multisample companion and the depth buffer hang off that one
// VulkanTextureData, so OffscreenTarget's msaaTexture and depthTexture and the
// drawable constructor's msaa and depth arguments all stay null here where
// D3D12 fills all three, and beginPass(descriptor) is one body.
//
// What the two do differently is how they end. An off-screen frame is rendered
// to be read, so its destructor submits and waits. A drawable frame submits
// with the acquire semaphore waited on and the render-finished semaphore
// signalled, then presents - and waits for nothing at all, the whole point of
// the semaphores being that the CPU never blocks on the GPU per frame. The
// throttle that keeps the CPU from running away is GPUView's, on the timeline.

namespace eacp::GPU
{
namespace
{
// Whether the driver will resolve a multisampled depth buffer by taking sample
// zero, which is what a sampleable depth on a multisampled target needs and
// what Metal's MTLMultisampleDepthResolveFilterSample0 does - so the two
// backends hand a shader the same value.
//
// The spec requires both masks to contain it, so this is a check on the driver
// rather than a branch anyone should ever take. A device that failed it would
// get a pass that attaches the depth buffer and records no resolve: the
// resolved image is still moved to where a bind can read it, so
// setFragmentDepthTexture stays legal, and what it reads is undefined rather
// than the frame's depth. Refusing the target at creation would be the other
// choice, and is not made until a device is found that needs it.
bool vulkanResolvesDepthBySampleZero()
{
    static const auto supported = []
    {
        auto physical = getVulkanShared().getPhysicalDevice();

        if (physical == VK_NULL_HANDLE)
            return false;

        VkPhysicalDeviceDepthStencilResolveProperties resolve = {};
        resolve.sType =
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DEPTH_STENCIL_RESOLVE_PROPERTIES;

        VkPhysicalDeviceProperties2 properties = {};
        properties.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
        properties.pNext = &resolve;

        vkGetPhysicalDeviceProperties2(physical, &properties);

        const auto sampleZero = VkResolveModeFlags {VK_RESOLVE_MODE_SAMPLE_ZERO_BIT};

        return (resolve.supportedDepthResolveModes & sampleZero) != 0
               && (resolve.supportedStencilResolveModes & sampleZero) != 0;
    }();

    return supported;
}

// The load and store one DepthAction names, which is the same pair for the
// depth plane and the stencil plane - both APIs put them in one attachment, so
// there is nothing here that is true of one and not the other.
//
// Clear discards on the way out, for the reason DepthAction says: a tile-based
// GPU would otherwise write the whole buffer back to memory at the end of every
// pass for pixels nothing reads. Resume is LOAD_OP_LOAD and is *not* Vulkan's
// suspending/resuming render pass, which is a different feature about splitting
// one pass across command buffers.
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
    // The swapchain image GPUView has just acquired. The msaa and depth
    // arguments are unread on this backend: both companions belong to the
    // drawable's own VulkanTextureData, exactly as they belong to the texture
    // on the off-screen path.
    Native(Device& deviceToUse, void* drawablePointer, void*, void*)
        : device(&deviceToUse)
        , drawable(static_cast<VulkanDrawable*>(drawablePointer))
    {
        if (drawable == nullptr || drawable->target == nullptr)
            return;

        target = drawable->target;

        // The acquire is the first submission's to wait on, wherever the first
        // submission turns out to be - this frame's last one if nothing
        // flushes, and flush()'s if something does.
        pendingWait = drawable->acquired;

        if (deviceToUse.isValid() && target->isValid() && target->isRenderTarget())
            open(context().acquire());
    }

    // The off-screen target: a texture the caller owns, rendered into and then
    // read back. The destructor waits for the GPU instead of presenting.
    Native(Device& deviceToUse, const OffscreenTarget& offscreenTarget)
        : device(&deviceToUse)
        , target(static_cast<VulkanTextureData*>(offscreenTarget.colorTexture))
        , offscreen(true)
    {
        if (deviceToUse.isValid() && target != nullptr && target->isValid()
            && target->isRenderTarget())
            open(context().acquire());
    }

    // Takes the recording and publishes it as the one a CPU upload may record
    // onto, for as long as this frame is the thing recording. Withdrawn in
    // ~Frame before anything is submitted, so an upload can never be handed a
    // command buffer that has already been ended.
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

        // Belt and braces: a pass always ends before its frame does, its
        // destructor seeing to that, and a flag left standing would send every
        // later upload to a recording of its own for nothing.
        context().setRenderPassOpen(false);
    }

    // The frame belongs to its Device's context: the recording came out of that
    // context's pool and is submitted back to it.
    VulkanContext& context() const { return getVulkanContext(*device); }

    VkCommandBuffer commandBuffer() const
    {
        return commands != nullptr ? commands->buffer : VK_NULL_HANDLE;
    }

    // The frame's opening timestamp and the query-pool reset, which have to be
    // the first things on the command buffer for the total to mean the frame.
    //
    // Called from Frame's constructor body rather than from this one, and the
    // order is the whole point: Device::beginFrame() is what gives the timer
    // the slot to write into, and it runs after every member is built.
    void beginTiming()
    {
        if (commands != nullptr)
            device->frameTimer().beginRecording(commands->buffer);
    }

    // Opens a timed pass and hands the encoder what it needs to close it when
    // the pass ends. Templated over the two encoder kinds because a render pass
    // and a compute pass are timed identically - a pair of timestamps on the
    // frame's own command buffer, in the order the work was recorded.
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

    // Both beginPass overloads land here: the drawable one has no target of its
    // own on this backend, and the texture one is the same pass over a texture
    // the caller named.
    RenderPass beginPassOn(VulkanTextureData& data,
                           const RenderPassDescriptor& descriptor)
    {
        if (commands == nullptr || !data.isValid() || !data.isRenderTarget())
            return RenderPass(nullptr);

        auto buffer = commands->buffer;

        // Everything the pass will read, made visible before it can no longer
        // be. See barrierBeforeRendering.
        barrierBeforeRendering(buffer);

        // The attachments moved in. Each helper records nothing where its image
        // is null, so this is the whole of the single-sampled, depth-less case
        // as well as the whole of the other seven.
        //
        // The texture itself takes imageColorAttachment either way: it is the
        // attachment on a single-sampled target and the resolve destination on
        // a multisampled one, and COLOR_ATTACHMENT_OPTIMAL is what a dynamic
        // rendering resolve wants for both sides.
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

        // Stored rather than discarded even when the samples are resolved away,
        // which is the one thing a multisampled target must not get wrong: a
        // second pass into it - a DepthAction::Resume pass, or anything drawing
        // on top of what is already there - loads the multisample image, and a
        // discard would hand it an empty one. Metal spells this
        // StoreAndMultisampleResolve for the same reason.
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

        // The driver's own depth resolve, into the single-sampled twin the
        // texture grew when sampleableDepth asked for one. Sample zero rather
        // than a min or a max: what reads this wants the depth of the surface
        // at the pixel, which is what a single-sampled render would have put
        // there.
        //
        // The stencil attachment below takes the same mode over the same view
        // deliberately. A device with independentResolveNone false requires the
        // two planes to resolve identically, and one plane resolving while the
        // other does not is exactly the combination that rule forbids.
        if (data.resolvedDepthAttachmentView != VK_NULL_HANDLE
            && vulkanResolvesDepthBySampleZero())
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

            // The same image, the same view and the same ops: one attachment
            // carries both planes, and what makes the stencil half exist is the
            // format the buffer was created with.
            if (data.hasStencil())
                rendering.pStencilAttachment = &depthAttachment;
        }

        // Before vkCmdBeginRendering so the clear the pass is about to do is
        // inside what the pass is measured as.
        auto* encoder =
            new VulkanRenderEncoder {commands, &data, data.width, data.height};
        timePass(*encoder, descriptor.label);

        vkCmdBeginRendering(buffer, &rendering);

        // From here until RenderPass::end an upload may not join this
        // recording: a copy inside a render pass instance is illegal, and so is
        // the barrier before it. See VulkanContext::getRecordingForCopy.
        context().setRenderPassOpen(true);

        // Viewport, scissor and stencil reference are the three dynamic states
        // every render pipeline on this backend declares, so all three have to
        // be set before the pass's first draw and none of them survives from
        // the pass before.
        //
        // **The viewport's height is negative**, which is the one axis Vulkan
        // differs from Metal and D3D12 on: clip-space y points down here with a
        // positive height, and flipping the viewport is what makes
        // VK_FRONT_FACE_COUNTER_CLOCKWISE mean what CullMode says eacp means by
        // it. See plan.md §3.5 for the two fixes that look equivalent and are
        // not.
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

        // Reset at every pass, because the reference is command-buffer state
        // here and encoder state on Metal - so a pass that sets one would
        // otherwise lend it to the next pass on the same frame and the two
        // backends would draw differently. Tests/GPU/StencilTests.cpp pins it.
        vkCmdSetStencilReference(buffer, VK_STENCIL_FACE_FRONT_AND_BACK, 0);

        return RenderPass(encoder, data.width, data.height);
    }

    // Hands the finished picture to the compositor. The queue mutex is the same
    // one every submit takes: presenting is a queue operation, and a VkQueue is
    // externally synchronized whichever call is using it.
    //
    // The wait is the render-finished semaphore the last submission signalled,
    // so this never blocks - vkQueuePresentKHR queues the wait rather than
    // performing it, and the presentation engine picks the image up when the
    // GPU is done with it.
    void present(std::uint64_t submittedValue)
    {
        // A submission the driver refused signals nothing, and a present
        // waiting on a semaphore that will never be signalled hangs the
        // compositor's view of this surface rather than dropping one frame.
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

    // The colour target: a texture the caller owns on the off-screen path, and
    // the acquired swapchain image on the drawable one.
    VulkanTextureData* target = nullptr;

    // Null on every frame but a drawable one, which is the test the destructor
    // and flush() branch on.
    VulkanDrawable* drawable = nullptr;

    // The acquire semaphore until some submission has taken it, and null after
    // - so a frame that flushes waits on the acquire once, in its first
    // submission, rather than in every one of them.
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
    // Nothing may record onto this command buffer from here on: what follows
    // ends it.
    impl->close();

    if (impl->commands == nullptr)
        return;

    auto& context = impl->context();

    // A frame that opened no pass never moved the image anywhere, and
    // vkQueuePresentKHR requires PRESENT_SRC_KHR. That is not a corner case: a
    // GPUView whose render() draws nothing is what the base class does, and its
    // first frame would otherwise be presented straight out of UNDEFINED.
    //
    // A frame that did open one is already resting there - RenderPass::end put
    // it there through restingUse() - so this records nothing, the barrier
    // helper skipping a transition to where the image already is.
    if (impl->drawable != nullptr && impl->target != nullptr)
        transitionTextureForUse(impl->commands->buffer, *impl->target, imagePresent);

    // The closing timestamp goes on while the buffer is still open; the
    // timeline value it will be read against only exists once it is submitted.
    impl->device->frameTimer().endFrame(impl->commands->buffer);

    // The acquire, if no earlier flush already took it, and the frame's
    // "picture finished" - so exactly one submission of the frame waits and
    // exactly one signals, whichever number of them there turned out to be.
    auto sync = SubmitSync {};

    if (impl->drawable != nullptr)
    {
        sync.wait = impl->pendingWait;
        sync.signal = impl->drawable->renderFinished;
        impl->pendingWait = VK_NULL_HANDLE;
    }

    const auto submitted = context.submit(impl->commands, sync);
    impl->device->frameTimer().noteSubmitted(submitted);

    // A drawable frame hands the image to the compositor and returns; the
    // waiting is the presentation engine's, which is the whole point of the two
    // semaphores. An off-screen frame is rendered to be read, and Texture::read
    // is only valid once what drew the texture has finished - so the wait is
    // the present's counterpart rather than an extra cost.
    if (impl->drawable != nullptr)
        impl->present(submitted);
    else if (impl->offscreen)
        context.waitIdle();
}

// The submit half of the destructor without the wait: the recording goes, the
// frame stays and takes a fresh one. Image layouts and buffer uses live on the
// resources rather than on the recording, so nothing the passes so far
// established is lost - and the next beginPass records its own barrier onto the
// new command buffer the way it records one onto every command buffer.
void Frame::flush()
{
    if (impl->commands == nullptr)
        return;

    auto& context = impl->context();

    // Withdrawn before the submit, exactly as ~Frame withdraws it: an upload
    // must never be handed a command buffer that is about to be ended.
    impl->close();

    // The acquire goes on this submission and on no later one. The
    // render-finished semaphore does not: it belongs to the *last* submission
    // of the frame, which by definition is not this one, or the present would
    // wait for a picture that is only half drawn.
    auto sync = SubmitSync {};
    sync.wait = impl->pendingWait;
    impl->pendingWait = VK_NULL_HANDLE;

    context.submit(impl->commands, sync);

    // The timer is not told, and needs no telling: its opening timestamp and
    // the pool reset are already on the queue, and the closing one goes onto
    // whichever command buffer is open when the frame ends. Both are queries on
    // one pool, executed in order, so the total still means the whole frame.
    impl->open(context.acquire());
}

RenderPass Frame::beginPass(const RenderPassDescriptor& descriptor)
{
    if (impl->target == nullptr)
        return RenderPass(nullptr);

    return impl->beginPassOn(*impl->target, descriptor);
}

// Rendering into an app-owned texture. Depth, multisampling and the sampleable
// depth twin are all the target's own, created with it, so this is the same
// pass the frame's own target gets - which is why there is one body.
//
// Passes on one frame are ordered by the queue, and the barrier each of them
// records before vkCmdBeginRendering is what makes a texture written by an
// earlier one legal to sample in a later one.
RenderPass Frame::beginPass(const Texture& target,
                            const RenderPassDescriptor& descriptor)
{
    auto* data = static_cast<VulkanTextureData*>(target.nativeTexture());

    if (data == nullptr || !target.isRenderTarget())
        return RenderPass(nullptr);

    return impl->beginPassOn(*data, descriptor);
}

// A compute pass on the frame's own recording, in order with its render passes
// and with nothing waiting in between. Compute and graphics bind points are
// separate on one command buffer, so a dispatch here disturbs nothing a pass
// bound - and a pass must have ended before this is called, a command buffer
// taking one encoder at a time.
ComputePass Frame::beginCompute(std::string_view label)
{
    if (impl->commands == nullptr)
        return ComputePass(nullptr);

    auto* encoder = new VulkanComputeEncoder {impl->commands};
    impl->timePass(*encoder, label);

    return ComputePass(encoder);
}

bool Frame::isValid() const
{
    return impl->commands != nullptr && impl->target != nullptr
           && impl->target->isValid();
}
} // namespace eacp::GPU
