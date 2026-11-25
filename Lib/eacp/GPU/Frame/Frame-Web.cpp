#include "Frame.h"

#include "../Device/Device.h"
#include "../Texture/MipChain.h"
#include "../WebGPU/WebGPUContext.h"

#include <eacp/Core/Utils/Logging.h>

#include <array>

namespace eacp::GPU
{
namespace
{
struct WebGPUClear
{
    Graphics::Color color = Graphics::Color::black();
    bool clear = true;
    int level = 0;
    int slice = 0;
    int layerCount = 1;
};

bool hasStencilPlane(wgpu::TextureFormat format)
{
    return format == wgpu::TextureFormat::Depth24PlusStencil8
           || format == wgpu::TextureFormat::Depth32FloatStencil8;
}

// A slice of a volume is attached through a 3D view and the depth slice it
// names; every other kind through a 2D or 2D-array view of its layers.
wgpu::TextureView colorView(const WebGPUTextureData& target, const WebGPUClear& at)
{
    if (target.kind != TextureType::ThreeD)
        return webgpuAttachmentView(
            target.texture, at.level, at.slice, at.layerCount);

    auto descriptor = wgpu::TextureViewDescriptor {};
    descriptor.dimension = wgpu::TextureViewDimension::e3D;
    descriptor.baseMipLevel = static_cast<std::uint32_t>(at.level);
    descriptor.mipLevelCount = 1;
    return target.texture.CreateView(&descriptor);
}

wgpu::RenderPassColorAttachment colorAttachmentFor(const WebGPUTextureData& target,
                                                   const WebGPUClear& at)
{
    auto attachment = wgpu::RenderPassColorAttachment {};
    const auto view = colorView(target, at);

    if (target.multisample != nullptr)
    {
        attachment.view = target.multisample.CreateView();
        attachment.resolveTarget = view;
    }
    else
    {
        attachment.view = view;
    }

    if (target.kind == TextureType::ThreeD)
        attachment.depthSlice = static_cast<std::uint32_t>(at.slice);

    attachment.loadOp = at.clear ? wgpu::LoadOp::Clear : wgpu::LoadOp::Load;
    attachment.storeOp = wgpu::StoreOp::Store;
    attachment.clearValue = {at.color.r, at.color.g, at.color.b, at.color.a};
    return attachment;
}

wgpu::TextureView
    depthAttachmentView(const WebGPUTextureData& data, int level, int slice)
{
    if (data.depth == nullptr)
        return {};

    const auto multisampled = data.sampleCount > 1;
    return webgpuAttachmentView(
        data.depth, multisampled ? 0 : level, multisampled ? 0 : slice);
}

TextureFormat drawableFormatOf(const WebGPUTextureData* target)
{
    return target != nullptr ? target->textureFormat : TextureFormat::BGRA8Unorm;
}
} // namespace

struct Frame::Native
{
    Native(Device& deviceToUse, void* drawablePointer, void*, void*)
        : device(&deviceToUse)
        , drawable(static_cast<WebGPUDrawable*>(drawablePointer))
    {
        if (drawable == nullptr || drawable->target == nullptr)
            return;

        target = drawable->target;

        if (deviceToUse.isValid() && target->isValid() && target->renderTarget)
            recording = context().beginRecording();
    }

    Native(Device& deviceToUse, const OffscreenTarget& offscreenTarget)
        : device(&deviceToUse)
        , target(static_cast<WebGPUTextureData*>(offscreenTarget.colorTexture))
    {
        if (deviceToUse.isValid() && target != nullptr && target->isValid()
            && target->renderTarget)
            recording = context().beginRecording();
    }

    WebGPUContext& context() const { return getWebGPUContext(*device); }

    RenderPass beginPassOn(std::span<WebGPUTextureData* const> targets,
                           std::span<const WebGPUClear> clears,
                           const RenderPassDescriptor& descriptor,
                           const WebGPUTextureData& depthData,
                           int depthLevel,
                           int depthSlice)
    {
        if (recording == nullptr || recording->passOpen)
            return RenderPass(nullptr);

        const auto limit =
            static_cast<std::size_t>(context().getLimits().maxColorAttachments);

        if (targets.size() > limit)
        {
            LOG("WebGPU: a pass over ",
                targets.size(),
                " targets is refused; this device offers ",
                limit);
            return RenderPass(nullptr);
        }

        auto attachments =
            std::array<wgpu::RenderPassColorAttachment, maxColorAttachments> {};

        for (auto index = std::size_t {0}; index < targets.size(); ++index)
        {
            if (!targets[index]->isValid() || !targets[index]->renderTarget)
                return RenderPass(nullptr);

            attachments[index] = colorAttachmentFor(*targets[index], clears[index]);
        }

        auto passDescriptor = wgpu::RenderPassDescriptor {};
        passDescriptor.label =
            wgpu::StringView {descriptor.label.data(), descriptor.label.size()};
        passDescriptor.colorAttachmentCount = targets.size();
        passDescriptor.colorAttachments = attachments.data();

        auto depthAttachment = wgpu::RenderPassDepthStencilAttachment {};
        const auto depthView =
            depthAttachmentView(depthData, depthLevel, depthSlice);

        if (depthView != nullptr)
        {
            const auto resumes = descriptor.depthAction == DepthAction::Resume;

            depthAttachment.view = depthView;
            depthAttachment.depthLoadOp =
                resumes ? wgpu::LoadOp::Load : wgpu::LoadOp::Clear;
            depthAttachment.depthStoreOp =
                descriptor.depthAction == DepthAction::Clear ? wgpu::StoreOp::Discard
                                                             : wgpu::StoreOp::Store;
            depthAttachment.depthClearValue = descriptor.clearDepth;

            if (hasStencilPlane(depthData.depthFormat))
            {
                depthAttachment.stencilLoadOp =
                    resumes ? wgpu::LoadOp::Load : wgpu::LoadOp::Clear;
                depthAttachment.stencilStoreOp = wgpu::StoreOp::Store;
                depthAttachment.stencilClearValue = descriptor.clearStencil;
            }

            passDescriptor.depthStencilAttachment = &depthAttachment;
        }

        const auto& first = *targets[0];
        const auto width = mipExtent(first.width, clears[0].level);
        const auto height = mipExtent(first.height, clears[0].level);

        auto* encoder = new WebGPURenderEncoder {
            &context(),
            recording.get(),
            recording->encoder.BeginRenderPass(&passDescriptor),
            depthView != nullptr ? depthData.depthFormat
                                 : wgpu::TextureFormat::Undefined,
            width,
            height};

        recording->passOpen = true;
        return RenderPass(
            encoder, width, height, Frame::occlusionQueriesFor(descriptor));
    }

    RenderPass beginPassOn(WebGPUTextureData& data,
                           const RenderPassDescriptor& descriptor)
    {
        WebGPUTextureData* const targets[] = {&data};
        const WebGPUClear clears[] = {{descriptor.clearColor, descriptor.clear}};
        return beginPassOn(targets, clears, descriptor, data, 0, 0);
    }

    Device* device = nullptr;
    WebGPUDrawable* drawable = nullptr;
    WebGPUTextureData* target = nullptr;
    OwningPointer<WebGPURecording> recording;
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
}

Frame::Frame(Device& device, const OffscreenTarget& target, float backingScaleToUse)
    : nesting(device, *this)
    , impl(device, target)
    , scale(backingScaleToUse)
{
    device.beginFrame();
}

Graphics::Point Frame::nativePixelSize() const
{
    if (impl->target == nullptr)
        return {};

    return {static_cast<float>(impl->target->width),
            static_cast<float>(impl->target->height)};
}

// The queue is in order, so a readback after this frame sees what it drew
// without waiting here; a drawable is presented by the page once the turn
// that rendered it returns.
Frame::~Frame()
{
    if (impl->recording != nullptr)
        impl->context().submit(*impl->recording);
}

void Frame::flush()
{
    if (impl->recording == nullptr)
        return;

    impl->context().submit(*impl->recording);
    impl->recording = impl->context().beginRecording();
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

    auto data = std::array<WebGPUTextureData*, maxColorAttachments> {};
    auto clears = std::array<WebGPUClear, maxColorAttachments> {};

    for (auto index = std::size_t {0}; index < targets.size(); ++index)
    {
        const auto& target = targets[index];
        data[index] =
            static_cast<WebGPUTextureData*>(target.texture->nativeTexture());
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
        static_cast<WebGPUTextureData*>(depthFrom.texture->nativeTexture());

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
    if (impl->recording == nullptr || impl->recording->passOpen)
        return ComputePass(nullptr, order);

    auto descriptor = wgpu::ComputePassDescriptor {};
    descriptor.label = wgpu::StringView {label.data(), label.size()};

    auto* encoder = new WebGPUComputeEncoder {
        &impl->context(),
        impl->recording.get(),
        impl->recording->encoder.BeginComputePass(&descriptor)};

    impl->recording->passOpen = true;
    return ComputePass(encoder, order);
}

TextureFormat Frame::nativeDrawableFormat() const
{
    return drawableFormatOf(impl->target);
}

bool Frame::copyNativeDrawable(const Texture& destination)
{
    if (impl->recording == nullptr || impl->target == nullptr
        || !canCopyDrawableInto(destination))
        return false;

    if (impl->recording->passOpen)
    {
        LOG("eacp: copyDrawable while a pass is open; end the pass first");
        return false;
    }

    auto* data = static_cast<WebGPUTextureData*>(destination.nativeTexture());

    if (data == nullptr || !data->isValid())
        return false;

    auto source = wgpu::TexelCopyTextureInfo {};
    source.texture = impl->target->texture;

    auto to = wgpu::TexelCopyTextureInfo {};
    to.texture = data->texture;

    const auto size =
        wgpu::Extent3D {static_cast<std::uint32_t>(impl->target->width),
                        static_cast<std::uint32_t>(impl->target->height),
                        1};

    impl->recording->encoder.CopyTextureToTexture(&source, &to, &size);
    return true;
}

bool Frame::isNativeValid() const
{
    return impl->recording != nullptr && impl->target != nullptr
           && impl->target->isValid();
}

void Frame::resumeRecording() {}

void* Frame::nativeRecording() const
{
    return impl->recording.get();
}
} // namespace eacp::GPU
