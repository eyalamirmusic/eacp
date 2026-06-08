#import <Metal/Metal.h>
#import <QuartzCore/CAMetalLayer.h>

#include "Frame.h"

#include "../Device/Device.h"

#include <eacp/Core/ObjC/ObjC.h>

namespace eacp::GPU
{
struct Frame::Native
{
    Native(Device& device,
           void* drawableHandle,
           void* msaaTextureHandle,
           void* depthTextureHandle)
    {
        if (drawableHandle != nullptr)
            drawable.reset((__bridge NSObject<CAMetalDrawable>*) drawableHandle);

        if (msaaTextureHandle != nullptr)
            msaaTexture.reset((__bridge NSObject<MTLTexture>*) msaaTextureHandle);

        if (depthTextureHandle != nullptr)
            depthTexture.reset((__bridge NSObject<MTLTexture>*) depthTextureHandle);

        if (auto queue = (__bridge id<MTLCommandQueue>) device.nativeQueue())
            commandBuffer.reset((NSObject<MTLCommandBuffer>*) [queue commandBuffer]);
    }

    ObjC::Ptr<NSObject<CAMetalDrawable>> drawable;
    ObjC::Ptr<NSObject<MTLTexture>> msaaTexture;
    ObjC::Ptr<NSObject<MTLTexture>> depthTexture;
    ObjC::Ptr<NSObject<MTLCommandBuffer>> commandBuffer;
};

Frame::Frame(Device& device, void* drawable, void* msaaTexture, void* depthTexture)
    : impl(device, drawable, msaaTexture, depthTexture)
{
}

Frame::~Frame()
{
    auto buffer = impl->commandBuffer.get();
    auto target = impl->drawable.get();

    if (buffer == nil)
        return;

    // The layer presents with transaction, so commit, wait for the buffer to be
    // scheduled, then present the drawable as part of the current CATransaction.
    [buffer commit];

    if (target != nil)
    {
        [buffer waitUntilScheduled];
        [(id<CAMetalDrawable>) target present];
    }
}

RenderPass Frame::beginPass(const RenderPassDescriptor& descriptor)
{
    auto target = impl->drawable.get();
    auto buffer = impl->commandBuffer.get();

    if (target == nil || buffer == nil)
        return RenderPass(nullptr);

    auto passDescriptor = [MTLRenderPassDescriptor renderPassDescriptor];
    auto colorAttachment = passDescriptor.colorAttachments[0];

    if (auto msaa = impl->msaaTexture.get())
    {
        colorAttachment.texture = (id<MTLTexture>) msaa;
        colorAttachment.resolveTexture = ((id<CAMetalDrawable>) target).texture;
        colorAttachment.storeAction = MTLStoreActionMultisampleResolve;
    }
    else
    {
        colorAttachment.texture = ((id<CAMetalDrawable>) target).texture;
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

    auto encoder = [buffer renderCommandEncoderWithDescriptor:passDescriptor];
    return RenderPass((__bridge void*) encoder);
}

bool Frame::isValid() const
{
    return impl->drawable.get() != nil && impl->commandBuffer.get() != nil;
}
} // namespace eacp::GPU
