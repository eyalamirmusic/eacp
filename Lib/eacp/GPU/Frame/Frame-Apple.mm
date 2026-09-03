#import <Metal/Metal.h>
#import <QuartzCore/CAMetalLayer.h>

#include "Frame.h"

#include "../Device/Device.h"

#include <eacp/Core/ObjC/ObjC.h>

namespace eacp::GPU
{
namespace
{
// Whether a depth attachment also carries a stencil plane. Only the one
// combined format is ever created here (GPUView::setStencil, and
// TextureDescriptor::stencil), so this asks about that one rather than
// enumerating formats Metal has and eacp does not use.
bool hasStencilPlane(MTLPixelFormat format)
{
    return format == MTLPixelFormatDepth32Float_Stencil8;
}

// Attaches the frame's depth buffer, and its stencil plane when the format has
// one - the same texture named twice, which is how Metal spells a combined
// attachment.
//
// Both planes take the same load and the same store, from DepthAction: they are
// one attachment of one format, so a pass resuming one and clearing the other
// would be describing a buffer the hardware does not have. Deciding it here
// rather than at the two call sites is what keeps them in step.
//
// Shared by the drawable pass and the texture-target pass so the two cannot
// drift on which planes a pass gets or what they start from.
//
// `resolved` is the single-sampled buffer a multisampled target's sampleable
// depth lives in, and is null everywhere else. Where there is one, the depth
// plane resolves into it at the end of every pass as well as being kept, for
// exactly the reason the colour does: a pass cannot know whether what follows it
// is another pass or a read of what it drew. The stencil plane is not resolved -
// nothing samples it, and no filter over a count would mean anything.
void attachDepthStencil(MTLRenderPassDescriptor* passDescriptor,
                        id<MTLTexture> depth,
                        id<MTLTexture> resolved,
                        unsigned char clearStencil,
                        DepthAction action)
{
    const auto load =
        action == DepthAction::Resume ? MTLLoadActionLoad : MTLLoadActionClear;

    // Discarded unless a later pass is going to load it. A tile-based GPU keeps
    // the buffer in tile memory, so storing one that nothing reads back writes
    // the target's size again to memory at the end of every pass.
    const auto store = action == DepthAction::Clear ? MTLStoreActionDontCare
                                                    : MTLStoreActionStore;

    auto depthAttachment = passDescriptor.depthAttachment;
    depthAttachment.texture = depth;
    depthAttachment.loadAction = load;
    depthAttachment.storeAction = store;
    depthAttachment.clearDepth = 1.0;

    if (resolved != nil)
    {
        depthAttachment.resolveTexture = resolved;

        // Sample 0 rather than Min or Max. What reads this wants the depth of
        // the surface at the pixel, which is what a single-sampled render would
        // have put there; the nearest or the furthest of the samples at a
        // silhouette is neither of the two surfaces meeting in it.
        depthAttachment.depthResolveFilter =
            MTLMultisampleDepthResolveFilterSample0;

        depthAttachment.storeAction =
            store == MTLStoreActionStore ? MTLStoreActionStoreAndMultisampleResolve
                                         : MTLStoreActionMultisampleResolve;
    }

    if (! hasStencilPlane(depth.pixelFormat))
        return;

    auto stencilAttachment = passDescriptor.stencilAttachment;
    stencilAttachment.texture = depth;
    stencilAttachment.loadAction = load;
    stencilAttachment.storeAction = store;
    stencilAttachment.clearStencil = (uint32_t) clearStencil;
}
} // namespace

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

        init(msaaTextureHandle, depthTextureHandle);
    }

    Native(Device& deviceToUse, const OffscreenTarget& target)
        : device(&deviceToUse)
    {
        if (target.colorTexture != nullptr)
            colorTexture.reset((__bridge NSObject<MTLTexture>*) target.colorTexture);

        init(target.msaaTexture, target.depthTexture);
    }

    void init(void* msaaTextureHandle, void* depthTextureHandle)
    {
        if (msaaTextureHandle != nullptr)
            msaaTexture.reset((__bridge NSObject<MTLTexture>*) msaaTextureHandle);

        if (depthTextureHandle != nullptr)
            depthTexture.reset((__bridge NSObject<MTLTexture>*) depthTextureHandle);

        openCommandBuffer();
    }

    // The buffer everything is recorded onto, taken again by flush() so that
    // recording continues after a submission rather than ending with it.
    void openCommandBuffer()
    {
        if (device == nullptr)
            return;

        if (auto queue = (__bridge id<MTLCommandQueue>) device->nativeQueue())
            commandBuffer.reset((NSObject<MTLCommandBuffer>*) [queue commandBuffer]);
    }

    // Points a pass descriptor's samples at this frame's slot in the timer.
    //
    // Only the two outer stage boundaries are sampled: a render pass starts at
    // the top of its vertex work and ends at the bottom of its fragment work,
    // and the two in between would say where the middle was rather than how
    // long the whole took.
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

    // Recorded before the commit, so a Buffer::read that follows this frame
    // waits for it. A presented frame only waits to be *scheduled*, which says
    // nothing about a compute pass on it having run.
    if (impl->device != nullptr)
    {
        impl->device->trackSubmittedWork((__bridge void*) buffer);

        // Also before the commit, though for a different reason: the timer
        // retains the command buffer here, and once committed it may finish -
        // and be asked for its GPU times - at any moment.
        impl->device->frameTimer().endFrame((__bridge void*) buffer);
    }

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

// The commit half of the destructor without the present: the work goes, the
// frame stays. A fresh command buffer takes over, and since the queue is FIFO
// it lands behind what was just sent - so a read-back committed in between sees
// everything recorded up to here and none of what follows.
void Frame::flush()
{
    auto buffer = impl->commandBuffer.get();

    if (buffer == nil)
        return;

    // Tracked for the same reason the destructor tracks it: a Buffer::read
    // after this waits for the work rather than for nothing.
    if (impl->device != nullptr)
        impl->device->trackSubmittedWork((__bridge void*) buffer);

    [buffer commit];

    // Not told to the timer, which takes one buffer per frame and reads the
    // total off it. The passes already recorded keep their samples - those live
    // in the device's sample buffer, not in the command buffer - and the total
    // becomes the part of the frame after this call. Frame.h says so.
    impl->openCommandBuffer();
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
        attachDepthStencil(passDescriptor,
                           (id<MTLTexture>) depth,
                           nil,
                           descriptor.clearStencil,
                           descriptor.depthAction);

    impl->timeRenderPass(passDescriptor, descriptor.label);

    auto encoder = [buffer renderCommandEncoderWithDescriptor:passDescriptor];

    // The pass carries the target's pixel size so it can clamp scissor rects;
    // the MSAA texture, when there is one, matches the resolve target's size.
    return RenderPass((__bridge void*) encoder,
                      (int) target.width,
                      (int) target.height);
}

// Rendering into an app-owned texture. Depth comes from the target when it was
// created with one, loaded and stored exactly as the drawable pass does it.
// Passes on one command buffer are ordered by the queue, so nothing here has to
// say that a later pass may sample what this one wrote.
//
// **A multisampled target renders into its multisample texture and resolves into
// itself**, which is the drawable path's shape with the resolve landing in an
// app-owned texture rather than in a drawable. Two things differ from the
// drawable's, and both follow from the target being readable between passes:
// the store is StoreAndMultisampleResolve rather than MultisampleResolve, so the
// samples survive for the next pass to load; and a pass that does not clear
// loads the *multisample* texture, which is the attachment, rather than the
// flattened picture it has been resolving into. See
// TextureDescriptor::sampleCount.
RenderPass Frame::beginPass(const Texture& target,
                            const RenderPassDescriptor& descriptor)
{
    auto buffer = impl->commandBuffer.get();
    auto texture = (__bridge id<MTLTexture>) target.nativeTexture();

    if (buffer == nil || texture == nil || !target.isRenderTarget())
        return RenderPass(nullptr);

    auto passDescriptor = [MTLRenderPassDescriptor renderPassDescriptor];
    auto colorAttachment = passDescriptor.colorAttachments[0];

    if (auto multisample =
            (__bridge id<MTLTexture>) target.nativeMultisampleTexture())
    {
        colorAttachment.texture = multisample;
        colorAttachment.resolveTexture = texture;
        colorAttachment.storeAction = MTLStoreActionStoreAndMultisampleResolve;
    }
    else
    {
        colorAttachment.texture = texture;
        colorAttachment.storeAction = MTLStoreActionStore;
    }

    colorAttachment.loadAction =
        descriptor.clear ? MTLLoadActionClear : MTLLoadActionLoad;

    const auto& color = descriptor.clearColor;
    colorAttachment.clearColor =
        MTLClearColorMake(color.r, color.g, color.b, color.a);

    if (auto depth = (__bridge id<MTLTexture>) target.nativeDepthTexture())
        attachDepthStencil(
            passDescriptor,
            depth,
            (__bridge id<MTLTexture>) target.nativeResolvedDepthTexture(),
            descriptor.clearStencil,
            descriptor.depthAction);

    impl->timeRenderPass(passDescriptor, descriptor.label);

    auto encoder = [buffer renderCommandEncoderWithDescriptor:passDescriptor];

    return RenderPass((__bridge void*) encoder,
                      (int) texture.width,
                      (int) texture.height);
}

// A compute pass samples at the encoder's own boundaries rather than at a
// vertex and a fragment stage, which is the same pair of numbers by another
// name: when the work started and when it finished.
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
