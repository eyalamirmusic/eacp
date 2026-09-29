#include "Frame.h"

#include "../Device/Device.h"
#include "../WebGPU/WebGPUTypes.h"

namespace eacp::GPU
{
namespace
{
WGPULoadOp webDepthLoadOp(DepthAction action)
{
    return action == DepthAction::Resume ? WGPULoadOp_Load : WGPULoadOp_Clear;
}

WGPUStoreOp webDepthStoreOp(DepthAction action)
{
    return action == DepthAction::Clear ? WGPUStoreOp_Discard : WGPUStoreOp_Store;
}
} // namespace

struct Frame::Native
{
    Native(Device& deviceToUse, void* drawablePointer, void*, void*)
        : device(&deviceToUse)
        , target(static_cast<WebTextureData*>(drawablePointer))
    {
        if (deviceToUse.isValid() && target != nullptr && target->isRenderTarget())
            encoder = context().beginRecording();
    }

    Native(Device& deviceToUse, const OffscreenTarget& offscreenTarget)
        : device(&deviceToUse)
        , target(static_cast<WebTextureData*>(offscreenTarget.colorTexture))
    {
        if (deviceToUse.isValid() && target != nullptr && target->isRenderTarget())
            encoder = context().beginRecording();
    }

    WebGPUContext& context() const { return getWebGPUContext(*device); }

    RenderPass beginPassOn(WebTextureData& data,
                           const RenderPassDescriptor& descriptor)
    {
        if (encoder == nullptr || !data.isRenderTarget())
            return RenderPass(nullptr);

        const auto& color = descriptor.clearColor;

        // Stored even when the samples are resolved away: a second pass into
        // the target loads the multisample texture, not the resolve.
        auto colorAttachment = WGPU_RENDER_PASS_COLOR_ATTACHMENT_INIT;
        colorAttachment.view = data.colorAttachmentView();
        colorAttachment.resolveTarget =
            data.isMultisampled() ? data.attachmentView : nullptr;
        colorAttachment.loadOp =
            descriptor.clear ? WGPULoadOp_Clear : WGPULoadOp_Load;
        colorAttachment.storeOp = WGPUStoreOp_Store;
        colorAttachment.clearValue = {color.r, color.g, color.b, color.a};

        auto depthAttachment = WGPU_RENDER_PASS_DEPTH_STENCIL_ATTACHMENT_INIT;
        depthAttachment.view = data.depthView;
        depthAttachment.depthLoadOp = webDepthLoadOp(descriptor.depthAction);
        depthAttachment.depthStoreOp = webDepthStoreOp(descriptor.depthAction);
        depthAttachment.depthClearValue = 1.f;

        if (data.hasStencil())
        {
            depthAttachment.stencilLoadOp = webDepthLoadOp(descriptor.depthAction);
            depthAttachment.stencilStoreOp = webDepthStoreOp(descriptor.depthAction);
            depthAttachment.stencilClearValue = descriptor.clearStencil;
        }

        auto passDescriptor = WGPU_RENDER_PASS_DESCRIPTOR_INIT;
        passDescriptor.label = toWebString(descriptor.label);
        passDescriptor.colorAttachmentCount = 1;
        passDescriptor.colorAttachments = &colorAttachment;

        auto timestamps = WGPU_PASS_TIMESTAMP_WRITES_INIT;

        if (timePass(timestamps, descriptor.label))
            passDescriptor.timestampWrites = &timestamps;

        if (data.hasDepth())
            passDescriptor.depthStencilAttachment = &depthAttachment;

        auto pass = wgpuCommandEncoderBeginRenderPass(encoder, &passDescriptor);

        if (pass == nullptr)
            return RenderPass(nullptr);

        auto* renderEncoder = new WebRenderEncoder {};
        renderEncoder->pass = pass;
        renderEncoder->context = &context();
        renderEncoder->target = &data;
        renderEncoder->width = data.width;
        renderEncoder->height = data.height;
        renderEncoder->depthFormat =
            data.hasDepth() ? data.depthFormat : WGPUTextureFormat_Undefined;

        return RenderPass(renderEncoder, data.width, data.height);
    }

    // The pass's own pair of timestamps, where the device has them.
    bool timePass(WGPUPassTimestampWrites& writes, std::string_view label)
    {
        auto& timer = device->frameTimer();
        const auto pass = timer.beginPass(label);
        auto querySet = static_cast<WGPUQuerySet>(timer.nativeSamples());

        if (pass < 0 || querySet == nullptr)
            return false;

        writes.querySet = querySet;
        writes.beginningOfPassWriteIndex = static_cast<std::uint32_t>(pass * 2);
        writes.endOfPassWriteIndex = static_cast<std::uint32_t>(pass * 2 + 1);

        return true;
    }

    Device* device = nullptr;
    WGPUCommandEncoder encoder = nullptr;
    WebTextureData* target = nullptr;
};

Frame::Frame(Device& device,
             void* drawable,
             void* msaaTexture,
             void* depthTexture,
             float backingScaleToUse)
    : impl(device, drawable, msaaTexture, depthTexture)
    , scale(backingScaleToUse)
{
    device.beginFrame();
}

Frame::Frame(Device& device, const OffscreenTarget& target, float backingScaleToUse)
    : impl(device, target)
    , scale(backingScaleToUse)
{
    device.beginFrame();
}

Graphics::Point Frame::pixelSize() const
{
    if (impl->target == nullptr)
        return {};

    return {static_cast<float>(impl->target->width),
            static_cast<float>(impl->target->height)};
}

// The drawable presents itself when control goes back to the browser: a canvas
// shows whatever its current texture held at the end of the task.
Frame::~Frame()
{
    if (impl->encoder == nullptr)
        return;

    auto& timer = impl->device->frameTimer();
    timer.endFrame(impl->encoder);

    timer.noteSubmitted(impl->context().submit(impl->encoder));
    impl->encoder = nullptr;
}

void Frame::flush()
{
    if (impl->encoder == nullptr)
        return;

    auto& context = impl->context();

    context.submit(impl->encoder);
    impl->encoder = context.beginRecording();
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
    auto* data = static_cast<WebTextureData*>(target.nativeTexture());

    if (data == nullptr || !target.isRenderTarget())
        return RenderPass(nullptr);

    return impl->beginPassOn(*data, descriptor);
}

ComputePass Frame::beginCompute(std::string_view label, DispatchOrder order)
{
    if (impl->encoder == nullptr)
        return ComputePass(nullptr, order);

    auto descriptor = WGPU_COMPUTE_PASS_DESCRIPTOR_INIT;
    descriptor.label = toWebString(label);

    auto timestamps = WGPU_PASS_TIMESTAMP_WRITES_INIT;

    if (impl->timePass(timestamps, label))
        descriptor.timestampWrites = &timestamps;

    auto* encoder = new WebComputeEncoder {};
    encoder->pass = wgpuCommandEncoderBeginComputePass(impl->encoder, &descriptor);
    encoder->context = &impl->context();

    return ComputePass(encoder, order);
}

bool Frame::isValid() const
{
    return impl->encoder != nullptr && impl->target != nullptr
           && impl->target->isValid();
}
} // namespace eacp::GPU
