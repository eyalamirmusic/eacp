#import <Metal/Metal.h>
#import <QuartzCore/CAMetalLayer.h>

#include "Frame.h"

#include "../Device/Device.h"

#include <eacp/Core/ObjC/ObjC.h>

namespace eacp::GPU
{
struct Frame::Native
{
    Native(Device& deviceToUse,
           void* drawableHandle,
           void* msaaTextureHandle,
           void* depthTextureHandle)
        : device(&deviceToUse)
    {
        if (drawableHandle != nullptr)
            drawable.reset((__bridge NSObject<CAMetalDrawable>*) drawableHandle);

        init(deviceToUse, msaaTextureHandle, depthTextureHandle);
    }

    Native(Device& deviceToUse, const OffscreenTarget& target)
        : device(&deviceToUse)
    {
        if (target.colorTexture != nullptr)
            colorTexture.reset((__bridge NSObject<MTLTexture>*) target.colorTexture);

        init(deviceToUse, target.msaaTexture, target.depthTexture);
    }

    void init(Device& device, void* msaaTextureHandle, void* depthTextureHandle)
    {
        if (msaaTextureHandle != nullptr)
            msaaTexture.reset((__bridge NSObject<MTLTexture>*) msaaTextureHandle);

        if (depthTextureHandle != nullptr)
            depthTexture.reset((__bridge NSObject<MTLTexture>*) depthTextureHandle);

        if (auto queue = (__bridge id<MTLCommandQueue>) device.nativeQueue())
            commandBuffer.reset((NSObject<MTLCommandBuffer>*) [queue commandBuffer]);
    }

    // Samples only the outer stage boundaries — start of vertex, end of
    // fragment — so the pair spans the whole pass.
    void timeRenderPass(MTLRenderPassDescriptor* passDescriptor,
                        std::string_view label) const
    {
        if (device == nullptr)
            return;

        auto& timer = device->frameTimer();
        const auto pass = timer.beginPass(label);

        if (pass < 0)
            return;

        auto samples = (__bridge id<MTLCounterSampleBuffer>) timer.nativeSamples();

        if (samples == nil)
            return;

        auto attachment = passDescriptor.sampleBufferAttachments[0];

        attachment.sampleBuffer = samples;
        attachment.startOfVertexSampleIndex = (NSUInteger) (pass * 2);
        attachment.endOfVertexSampleIndex = MTLCounterDontSample;
        attachment.startOfFragmentSampleIndex = MTLCounterDontSample;
        attachment.endOfFragmentSampleIndex = (NSUInteger) (pass * 2 + 1);
    }

    id<MTLTexture> storeTexture() const
    {
        if (auto d = drawable.get())
            return ((id<CAMetalDrawable>) d).texture;

        return (id<MTLTexture>) colorTexture.get();
    }

    ObjC::Ptr<NSObject<CAMetalDrawable>> drawable;
    ObjC::Ptr<NSObject<MTLTexture>> colorTexture;
    ObjC::Ptr<NSObject<MTLTexture>> msaaTexture;
    ObjC::Ptr<NSObject<MTLTexture>> depthTexture;
    ObjC::Ptr<NSObject<MTLCommandBuffer>> commandBuffer;
    Device* device = nullptr;
};

Frame::Frame(Device& device, void* drawable, void* msaaTexture, void* depthTexture)
    : impl(device, drawable, msaaTexture, depthTexture)
{
    device.beginFrame();
}

Frame::Frame(Device& device, const OffscreenTarget& target)
    : impl(device, target)
{
    device.beginFrame();
}

Frame::~Frame()
{
    auto buffer = impl->commandBuffer.get();
    auto target = impl->drawable.get();

    if (buffer == nil)
        return;

    // Both must precede the commit: presenting only waits for the buffer to be
    // scheduled, and once committed it may finish at any moment.
    if (impl->device != nullptr)
    {
        impl->device->trackSubmittedWork((__bridge void*) buffer);
        impl->device->frameTimer().endFrame((__bridge void*) buffer);
    }

    if (target != nil)
    {
        // The layer presents with transaction, so the drawable must be presented
        // inside the CATransaction, once the buffer is scheduled.
        [buffer commit];
        [buffer waitUntilScheduled];
        [(id<CAMetalDrawable>) target present];
    }
    else
    {
        // Blocks so the colour texture can be read back on return.
        [buffer commit];
        [buffer waitUntilCompleted];
    }
}

RenderPass Frame::beginPass(const RenderPassDescriptor& descriptor)
{
    auto target = impl->storeTexture();
    auto buffer = impl->commandBuffer.get();

    if (target == nil || buffer == nil)
        return RenderPass(nullptr);

    auto passDescriptor = [MTLRenderPassDescriptor renderPassDescriptor];
    auto colorAttachment = passDescriptor.colorAttachments[0];

    if (auto msaa = impl->msaaTexture.get())
    {
        colorAttachment.texture = (id<MTLTexture>) msaa;
        colorAttachment.resolveTexture = target;
        colorAttachment.storeAction = MTLStoreActionMultisampleResolve;
    }
    else
    {
        colorAttachment.texture = target;
        colorAttachment.storeAction = MTLStoreActionStore;
    }

    colorAttachment.loadAction =
        descriptor.clear ? MTLLoadActionClear : MTLLoadActionLoad;

    const auto& color = descriptor.clearColor;
    colorAttachment.clearColor =
        MTLClearColorMake(color.r, color.g, color.b, color.a);

    if (auto depth = impl->depthTexture.get())
    {
        auto depthAttachment = passDescriptor.depthAttachment;
        depthAttachment.texture = (id<MTLTexture>) depth;
        depthAttachment.loadAction = MTLLoadActionClear;
        depthAttachment.storeAction = MTLStoreActionDontCare;
        depthAttachment.clearDepth = 1.0;
    }

    impl->timeRenderPass(passDescriptor, descriptor.label);

    auto encoder = [buffer renderCommandEncoderWithDescriptor:passDescriptor];

    // The pass carries the target's pixel size so it can clamp scissor rects.
    return RenderPass((__bridge void*) encoder,
                      (int) target.width,
                      (int) target.height);
}

RenderPass Frame::beginPass(const Texture& target,
                            const RenderPassDescriptor& descriptor)
{
    auto buffer = impl->commandBuffer.get();
    auto texture = (__bridge id<MTLTexture>) target.nativeTexture();

    if (buffer == nil || texture == nil || !target.isRenderTarget())
        return RenderPass(nullptr);

    auto passDescriptor = [MTLRenderPassDescriptor renderPassDescriptor];
    auto colorAttachment = passDescriptor.colorAttachments[0];

    colorAttachment.texture = texture;
    colorAttachment.storeAction = MTLStoreActionStore;
    colorAttachment.loadAction =
        descriptor.clear ? MTLLoadActionClear : MTLLoadActionLoad;

    const auto& color = descriptor.clearColor;
    colorAttachment.clearColor =
        MTLClearColorMake(color.r, color.g, color.b, color.a);

    if (auto depth = (__bridge id<MTLTexture>) target.nativeDepthTexture())
    {
        auto depthAttachment = passDescriptor.depthAttachment;
        depthAttachment.texture = depth;
        depthAttachment.loadAction = MTLLoadActionClear;
        depthAttachment.storeAction = MTLStoreActionDontCare;
        depthAttachment.clearDepth = 1.0;
    }

    impl->timeRenderPass(passDescriptor, descriptor.label);

    auto encoder = [buffer renderCommandEncoderWithDescriptor:passDescriptor];

    return RenderPass((__bridge void*) encoder,
                      (int) texture.width,
                      (int) texture.height);
}

// Samples at the encoder's own boundaries, there being no vertex or fragment
// stage to bracket.
ComputePass Frame::beginCompute(std::string_view label)
{
    auto buffer = impl->commandBuffer.get();

    if (buffer == nil)
        return ComputePass(nullptr);

    auto passDescriptor = [MTLComputePassDescriptor computePassDescriptor];

    if (impl->device != nullptr)
    {
        auto& timer = impl->device->frameTimer();
        const auto pass = timer.beginPass(label);

        if (pass >= 0)
        {
            if (auto samples =
                    (__bridge id<MTLCounterSampleBuffer>) timer.nativeSamples())
            {
                auto attachment = passDescriptor.sampleBufferAttachments[0];

                attachment.sampleBuffer = samples;
                attachment.startOfEncoderSampleIndex = (NSUInteger) (pass * 2);
                attachment.endOfEncoderSampleIndex = (NSUInteger) (pass * 2 + 1);
            }
        }
    }

    return ComputePass((__bridge void*) [(id<MTLCommandBuffer>) buffer
        computeCommandEncoderWithDescriptor:passDescriptor]);
}

bool Frame::isValid() const
{
    return impl->storeTexture() != nil && impl->commandBuffer.get() != nil;
}
} // namespace eacp::GPU
