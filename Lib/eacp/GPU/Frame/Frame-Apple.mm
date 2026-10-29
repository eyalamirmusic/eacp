#import <Metal/Metal.h>
#import <QuartzCore/CAMetalLayer.h>

#include "Frame.h"

#include "../CommandBuffer/CopyPlan.h"
#include "../Device/Device.h"
#include "../Device/MetalErrors-Apple.h"
#include "../Timing/PassSamples-Apple.h"

#include <eacp/Core/ObjC/ObjC.h>
#include <eacp/Core/Utils/Logging.h>

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

// Where an attachment points inside its texture. One image is the
// attachment's own level, slice and depthPlane; a run of layers is a 2D array
// view of them, so the vertex shader's layer index counts from the run's first
// slice on every backend - Metal's own slice would be ignored by a layered
// pass, which addresses the whole texture from 0.
struct AttachmentImage
{
    id<MTLTexture> texture = nil;
    NSUInteger level = 0;
    NSUInteger slice = 0;
    NSUInteger depthPlane = 0;
};

AttachmentImage attachmentImage(id<MTLTexture> texture,
                                int level,
                                int slice,
                                int layerCount)
{
    auto image = AttachmentImage {texture};

    if (texture == nil)
        return image;

    if (layerCount > 1 && texture.textureType != MTLTextureType3D)
    {
        image.texture = [texture
            newTextureViewWithPixelFormat:texture.pixelFormat
                              textureType:MTLTextureType2DArray
                                   levels:NSMakeRange((NSUInteger) level, 1)
                                   slices:NSMakeRange((NSUInteger) slice,
                                                      (NSUInteger) layerCount)];
        [image.texture autorelease];
        return image;
    }

    image.level = (NSUInteger) level;

    if (texture.textureType == MTLTextureType3D)
        image.depthPlane = (NSUInteger) slice;
    else
        image.slice = (NSUInteger) slice;

    return image;
}

void point(MTLRenderPassAttachmentDescriptor* attachment, const AttachmentImage& image)
{
    attachment.texture = image.texture;
    attachment.level = image.level;
    attachment.slice = image.slice;
    attachment.depthPlane = image.depthPlane;
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
                        float clearDepth,
                        unsigned char clearStencil,
                        DepthAction action,
                        const ColorTarget& at = {})
{
    const auto image = attachmentImage(depth, at.level, at.slice, at.layerCount);

    const auto load =
        action == DepthAction::Resume ? MTLLoadActionLoad : MTLLoadActionClear;

    // Discarded unless a later pass is going to load it. A tile-based GPU keeps
    // the buffer in tile memory, so storing one that nothing reads back writes
    // the target's size again to memory at the end of every pass.
    const auto store = action == DepthAction::Clear ? MTLStoreActionDontCare
                                                    : MTLStoreActionStore;

    auto depthAttachment = passDescriptor.depthAttachment;
    point(depthAttachment, image);
    depthAttachment.loadAction = load;
    depthAttachment.storeAction = store;
    depthAttachment.clearDepth = (double) clearDepth;

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
    point(stencilAttachment, image);
    stencilAttachment.loadAction = load;
    stencilAttachment.storeAction = store;
    stencilAttachment.clearStencil = (uint32_t) clearStencil;
}
void attachColor(MTLRenderPassColorAttachmentDescriptor* attachment,
                 const ColorTarget& target)
{
    auto texture = (__bridge id<MTLTexture>) target.texture->nativeTexture();

    if (auto multisample =
            (__bridge id<MTLTexture>) target.texture->nativeMultisampleTexture())
    {
        attachment.texture = multisample;
        attachment.resolveTexture = texture;
        attachment.storeAction = MTLStoreActionStoreAndMultisampleResolve;
    }
    else
    {
        point(attachment,
              attachmentImage(texture, target.level, target.slice, target.layerCount));
        attachment.storeAction = MTLStoreActionStore;
    }

    attachment.loadAction = target.clear ? MTLLoadActionClear : MTLLoadActionLoad;

    const auto& color = target.clearColor;
    attachment.clearColor = MTLClearColorMake(color.r, color.g, color.b, color.a);
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
        {
            commandBuffer.reset((NSObject<MTLCommandBuffer>*) [queue commandBuffer]);
            Detail::reportFailureOf((id<MTLCommandBuffer>) commandBuffer.get());
        }
    }

    // Points a pass descriptor's samples at this frame's slot in the timer.
    //
    // All four stage boundaries, not only the outer two: a tile-based GPU runs
    // a pass's vertex stage while the passes before it are still shading, so
    // its start and end alone would span their time as well as its own.
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

        PassSamples::attach(passDescriptor, samples, pass);
    }

    // Names the set's buffer as the pass's visibility results, which Metal only
    // takes on the descriptor, and tells the set which command buffer will
    // write it. Returns the set, or null when the pass counts nothing.
    OcclusionQueries* countOcclusionInto(MTLRenderPassDescriptor* passDescriptor,
                                         const RenderPassDescriptor& descriptor) const
    {
        auto* queries = Frame::occlusionQueriesFor(descriptor);

        if (queries == nullptr)
            return nullptr;

        passDescriptor.visibilityResultBuffer =
            (__bridge id<MTLBuffer>) queries->nativeResults();
        queries->notePass((__bridge void*) commandBuffer.get());
        return queries;
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
    auto target = impl->storeTexture();

    if (target == nil)
        return {};

    return {(float) target.width, (float) target.height};
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

RenderPass Frame::beginNativePass(const RenderPassDescriptor& descriptor)
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
                           descriptor.clearDepth,
                           descriptor.clearStencil,
                           descriptor.depthAction);

    impl->timeRenderPass(passDescriptor, descriptor.label);
    auto* queries = impl->countOcclusionInto(passDescriptor, descriptor);

    auto encoder = [buffer renderCommandEncoderWithDescriptor:passDescriptor];

    // The pass carries the target's pixel size so it can clamp scissor rects;
    // the MSAA texture, when there is one, matches the resolve target's size.
    return RenderPass((__bridge void*) encoder,
                      (int) target.width,
                      (int) target.height,
                      queries);
}

// Rendering into app-owned textures. Depth comes from the first target when it
// was created with one, loaded and stored exactly as the drawable pass does it.
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
RenderPass Frame::beginMultiTargetPass(std::span<const ColorTarget> targets,
                            const RenderPassDescriptor& descriptor)
{
    auto buffer = impl->commandBuffer.get();

    if (buffer == nil || !canRenderInto(targets, descriptor))
        return RenderPass(nullptr);

    auto passDescriptor = [MTLRenderPassDescriptor renderPassDescriptor];

    for (auto index = std::size_t {0}; index < targets.size(); ++index)
        attachColor(passDescriptor.colorAttachments[index], targets[index]);

    const auto& first = *targets[0].texture;
    const auto depthFrom = depthSource(targets, descriptor);
    const auto& depthOwner = *depthFrom.texture;

    if (auto depth = (__bridge id<MTLTexture>) depthOwner.nativeDepthTexture())
        attachDepthStencil(
            passDescriptor,
            depth,
            (__bridge id<MTLTexture>) depthOwner.nativeResolvedDepthTexture(),
            descriptor.clearDepth,
            descriptor.clearStencil,
            descriptor.depthAction,
            depthFrom);

    if (targets[0].layerCount > 1)
        passDescriptor.renderTargetArrayLength = (NSUInteger) targets[0].layerCount;

    impl->timeRenderPass(passDescriptor, descriptor.label);
    auto* queries = impl->countOcclusionInto(passDescriptor, descriptor);

    auto encoder = [buffer renderCommandEncoderWithDescriptor:passDescriptor];

    return RenderPass((__bridge void*) encoder,
                      levelWidth(first, targets[0].level),
                      levelHeight(first, targets[0].level),
                      queries);
}

// A compute pass samples at the encoder's own boundaries, since it has no
// vertex and fragment stage to split: when the work started and when it
// finished.
ComputePass Frame::beginCompute(std::string_view label, DispatchOrder order)
{
    auto buffer = impl->commandBuffer.get();

    if (buffer == nil)
        return ComputePass(nullptr, order);

    auto passDescriptor = [MTLComputePassDescriptor computePassDescriptor];

    if (order == DispatchOrder::Concurrent)
        passDescriptor.dispatchType = MTLDispatchTypeConcurrent;

    if (impl->device != nullptr)
    {
        auto& timer = impl->device->frameTimer();
        const auto pass = timer.beginPass(label);

        if (pass >= 0)
        {
            if (auto samples =
                    (__bridge id<MTLCounterSampleBuffer>) timer.nativeSamples())
                PassSamples::attach(passDescriptor, samples, pass);
        }
    }

    return ComputePass((__bridge void*) [(id<MTLCommandBuffer>) buffer
                           computeCommandEncoderWithDescriptor:passDescriptor],
                       order);
}

TextureFormat Frame::nativeDrawableFormat() const
{
    auto target = impl->storeTexture();

    if (target == nil)
        return TextureFormat::BGRA8Unorm;

    switch (target.pixelFormat)
    {
        case MTLPixelFormatRGBA8Unorm:
            return TextureFormat::RGBA8Unorm;
        case MTLPixelFormatRGBA8Unorm_sRGB:
            return TextureFormat::RGBA8UnormSrgb;
        case MTLPixelFormatBGRA8Unorm_sRGB:
            return TextureFormat::BGRA8UnormSrgb;
        case MTLPixelFormatRGBA16Float:
            return TextureFormat::RGBA16Float;
        default:
            return TextureFormat::BGRA8Unorm;
    }
}

// A blit on the frame's own command buffer, so it lands after the passes
// recorded before it and before whatever follows. A drawable's texture is
// only a blit source when its layer is not framebufferOnly, which is what
// GPUView::setReadableDrawable turns off; a snapshot's texture always is.
bool Frame::copyNativeDrawable(const Texture& destination)
{
    auto target = impl->storeTexture();
    auto buffer = impl->commandBuffer.get();

    if (target == nil || buffer == nil || !canCopyDrawableInto(destination))
        return false;

    if (target.framebufferOnly)
    {
        LOG("eacp: copyDrawable on a view whose drawable is not readable; see "
            "GPUView::setReadableDrawable");
        return false;
    }

    auto copyTarget = (__bridge id<MTLTexture>) destination.nativeTexture();
    auto blit = [(id<MTLCommandBuffer>) buffer blitCommandEncoder];

    [blit copyFromTexture:target
                 sourceSlice:0
                 sourceLevel:0
                sourceOrigin:MTLOriginMake(0, 0, 0)
                  sourceSize:MTLSizeMake(target.width, target.height, 1)
                   toTexture:copyTarget
            destinationSlice:0
            destinationLevel:0
           destinationOrigin:MTLOriginMake(0, 0, 0)];
    [blit endEncoding];

    return true;
}

bool Frame::isNativeValid() const
{
    return impl->storeTexture() != nil && impl->commandBuffer.get() != nil;
}

// Metal has no open recording for an upload to go onto: Texture::update and
// Buffer::update order themselves on the queue.
void Frame::resumeRecording()
{
}

void* Frame::nativeRecording() const
{
    return (__bridge void*) impl->commandBuffer.get();
}
} // namespace eacp::GPU
