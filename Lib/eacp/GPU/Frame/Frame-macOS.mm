#import <Metal/Metal.h>
#import <QuartzCore/CAMetalLayer.h>

#include "Frame.h"

#include "../Device/Device.h"

#include <eacp/Core/ObjC/ObjC.h>

namespace eacp::GPU
{
struct Frame::Native
{
    Native(Device& device, void* drawableHandle, void* msaaTextureHandle)
    {
        if (drawableHandle != nullptr)
            drawable.reset((__bridge NSObject<CAMetalDrawable>*) drawableHandle);

        if (msaaTextureHandle != nullptr)
            msaaTexture.reset((__bridge NSObject<MTLTexture>*) msaaTextureHandle);

        if (auto queue = (__bridge id<MTLCommandQueue>) device.nativeQueue())
            commandBuffer.reset((NSObject<MTLCommandBuffer>*) [queue commandBuffer]);
    }

    ObjC::Ptr<NSObject<CAMetalDrawable>> drawable;
    ObjC::Ptr<NSObject<MTLTexture>> msaaTexture;
    ObjC::Ptr<NSObject<MTLCommandBuffer>> commandBuffer;
};

Frame::Frame(Device& device, void* drawable, void* msaaTexture)
    : impl(device, drawable, msaaTexture)
{
}

Frame::~Frame()
{
    auto buffer = impl->commandBuffer.get();
    auto target = impl->drawable.get();

    if (buffer == nil)
        return;

    if (target != nil)
        [buffer presentDrawable:(id<CAMetalDrawable>) target];

    [buffer commit];
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

    auto encoder = [buffer renderCommandEncoderWithDescriptor:passDescriptor];
    return RenderPass((__bridge void*) encoder);
}

bool Frame::isValid() const
{
    return impl->drawable.get() != nil && impl->commandBuffer.get() != nil;
}
} // namespace eacp::GPU
