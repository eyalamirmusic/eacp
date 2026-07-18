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

        init(device, msaaTextureHandle, depthTextureHandle);
    }

    Native(Device& device, const OffscreenTarget& target)
    {
        if (target.colorTexture != nullptr)
            colorTexture.reset((__bridge NSObject<MTLTexture>*) target.colorTexture);

        init(device, target.msaaTexture, target.depthTexture);
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

    // The texture the pass stores into: the drawable's on-screen texture, or the
    // app-owned off-screen colour texture for a snapshot.
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
};

Frame::Frame(Device& device, void* drawable, void* msaaTexture, void* depthTexture)
    : impl(device, drawable, msaaTexture, depthTexture)
{
}

Frame::Frame(Device& device, const OffscreenTarget& target)
    : impl(device, target)
{
}

Frame::~Frame()
{
    auto buffer = impl->commandBuffer.get();
    auto target = impl->drawable.get();

    if (buffer == nil)
        return;

    if (target != nil)
    {
        // The layer presents with transaction, so commit, wait for the buffer to
        // be scheduled, then present the drawable inside the CATransaction.
        [buffer commit];
        [buffer waitUntilScheduled];
        [(id<CAMetalDrawable>) target present];
    }
    else
    {
        // Off-screen: block until the GPU finishes so the colour texture can be
        // read back on return.
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

    auto encoder = [buffer renderCommandEncoderWithDescriptor:passDescriptor];

    // The pass carries the target's pixel size so it can clamp scissor rects;
    // the MSAA texture, when there is one, matches the resolve target's size.
    return RenderPass((__bridge void*) encoder,
                      (int) target.width,
                      (int) target.height);
}

bool Frame::isValid() const
{
    return impl->storeTexture() != nil && impl->commandBuffer.get() != nil;
}
} // namespace eacp::GPU
