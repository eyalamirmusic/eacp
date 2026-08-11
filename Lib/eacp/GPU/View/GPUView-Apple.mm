#import <CoreVideo/CoreVideo.h>
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#import <QuartzCore/CAMetalLayer.h>

#include "GPUView.h"
#include "GPUViewBacking-Apple.h"

#include "../Device/Device.h"
#include "../Frame/Frame.h"

#include <eacp/Core/ObjC/ObjC.h>
#include <eacp/Graphics/Image/Image.h>
#include <eacp/Graphics/Layers/ImmediateLayerClass.h>
#include <eacp/Graphics/Primitives/GraphicUtils.h>

#include <algorithm>
#include <cmath>

namespace eacp::GPU
{
namespace
{
// Suppresses implicit Core Animation actions so the layer tracks the view's
// size instantly during live resize instead of animating to it.
Class getImmediateMetalLayerClass()
{
    static auto cls = Graphics::makeImmediateLayerClass<CAMetalLayer>(
        "EacpImmediateMetalLayer");
    return cls;
}
} // namespace

struct GPUView::Native
{
    explicit Native(GPUView& viewToUse)
        : view(viewToUse)
    {
        metalLayer = [[getImmediateMetalLayerClass() alloc] init];

        auto metalDevice = (__bridge id<MTLDevice>) Device::shared().nativeDevice();
        metalLayer.get().device = metalDevice;
        metalLayer.get().pixelFormat = MTLPixelFormatBGRA8Unorm;
        metalLayer.get().framebufferOnly = YES;

        // Presents inside the current CATransaction, so new content lands
        // atomically with the layer's new size during live resize.
        metalLayer.get().presentsWithTransaction = YES;

        metalLayer.get().maximumDrawableCount = framesInFlight;

        auto base = (__bridge CALayer*) view.getNativeLayer();
        [base addSublayer:metalLayer.get()];
    }

    ~Native() { [metalLayer.get() removeFromSuperlayer]; }

    void updateSize()
    {
        auto scale = platformBackingScale(view);

        auto bounds = Graphics::toCGRect(view.getLocalBounds());
        auto pixelWidth = (NSUInteger) (bounds.size.width * scale);
        auto pixelHeight = (NSUInteger) (bounds.size.height * scale);

        metalLayer.get().frame = bounds;
        metalLayer.get().contentsScale = scale;
        metalLayer.get().drawableSize = CGSizeMake(pixelWidth, pixelHeight);

        updateMultisampleTexture(pixelWidth, pixelHeight);
        updateDepthTexture(pixelWidth, pixelHeight);

        // Notified after the layer is consistent at the new scale, so a handler
        // sees the size it will draw into. Skipped on the first pass.
        const auto newScale = (float) scale;

        if (backingScale > 0.f && newScale != backingScale)
        {
            backingScale = newScale;
            view.onBackingScaleChanged(newScale);
        }
        else
        {
            backingScale = newScale;
        }
    }

    void updateMultisampleTexture(NSUInteger width, NSUInteger height)
    {
        if (sampleCount <= 1 || width == 0 || height == 0)
        {
            msaaTexture.release();
            return;
        }

        auto metalDevice = (__bridge id<MTLDevice>) Device::shared().nativeDevice();

        auto textureDescriptor = [MTLTextureDescriptor
            texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
                                         width:width
                                        height:height
                                     mipmapped:NO];
        textureDescriptor.textureType = MTLTextureType2DMultisample;
        textureDescriptor.sampleCount = (NSUInteger) sampleCount;
        textureDescriptor.usage = MTLTextureUsageRenderTarget;
        textureDescriptor.storageMode = MTLStorageModePrivate;

        msaaTexture = [metalDevice newTextureWithDescriptor:textureDescriptor];
    }

    void updateDepthTexture(NSUInteger width, NSUInteger height)
    {
        if (! depthEnabled || width == 0 || height == 0)
        {
            depthTexture.release();
            return;
        }

        auto metalDevice = (__bridge id<MTLDevice>) Device::shared().nativeDevice();

        auto textureDescriptor = [MTLTextureDescriptor
            texture2DDescriptorWithPixelFormat:MTLPixelFormatDepth32Float
                                         width:width
                                        height:height
                                     mipmapped:NO];

        // The depth attachment must match the colour attachment's sample count.
        if (sampleCount > 1)
        {
            textureDescriptor.textureType = MTLTextureType2DMultisample;
            textureDescriptor.sampleCount = (NSUInteger) sampleCount;
        }

        textureDescriptor.usage = MTLTextureUsageRenderTarget;
        textureDescriptor.storageMode = MTLStorageModePrivate;

        depthTexture = [metalDevice newTextureWithDescriptor:textureDescriptor];
    }

    void startContinuous()
    {
        if (displayLink == nullptr)
            displayLink = makeOwned<Threads::DisplayLink>(
                [this](Threads::FrameTime time)
                {
                    view.update(time);
                    view.renderNow();
                });
    }

    void stopContinuous() { displayLink = nullptr; }

    GPUView& view;
    int sampleCount = 4;

    // A drawable pool, not a queue: three keeps nextDrawable from blocking, and
    // lowering it costs latency. See GPUView::setFramesInFlight.
    int framesInFlight = 3;
    bool continuous = false;
    bool depthEnabled = false;

    // Zero until the first updateSize(), which tells the initial scale apart
    // from a change.
    float backingScale = 0.f;

    ObjC::Ptr<CAMetalLayer> metalLayer;
    ObjC::Ptr<NSObject<MTLTexture>> msaaTexture;
    ObjC::Ptr<NSObject<MTLTexture>> depthTexture;
    OwningPointer<Threads::DisplayLink> displayLink;
};

GPUView::GPUView()
    : impl(*this)
{
}

GPUView::~GPUView() = default;

int GPUView::sampleCount() const
{
    return impl->sampleCount;
}

void GPUView::setSampleCount(int count)
{
    impl->sampleCount = count;
}

void GPUView::setDepth(bool enabled)
{
    impl->depthEnabled = enabled;
    impl->updateSize();
}

bool GPUView::hasDepth() const
{
    return impl->depthEnabled;
}

void GPUView::setContinuous(bool continuous)
{
    impl->continuous = continuous;

    if (continuous)
        impl->startContinuous();
    else
        impl->stopContinuous();
}

bool GPUView::isContinuous() const
{
    return impl->continuous;
}

void GPUView::setFramesInFlight(int count)
{
    // Metal accepts two or three.
    impl->framesInFlight = count < 2 ? 2 : (count > 3 ? 3 : count);
    impl->metalLayer.get().maximumDrawableCount = impl->framesInFlight;
}

int GPUView::framesInFlight() const
{
    return impl->framesInFlight;
}

void GPUView::resized()
{
    Graphics::View::resized();
    impl->updateSize();

    // Synchronously within the resize pass, not a display link tick later.
    renderNow();
}

void GPUView::backingScaleChanged()
{
    Graphics::View::backingScaleChanged();

    // Redrawn because the presented frame was rasterized for the old scale.
    impl->updateSize();
    renderNow();
}

float GPUView::backingScale() const
{
    // updateSize() has not run before the view is first laid out.
    if (impl->backingScale > 0.f)
        return impl->backingScale;

    return (float) platformBackingScale(const_cast<GPUView&>(*this));
}

void GPUView::paint(Graphics::Context& context)
{
    // A snapshot goes through renderNativeContent; renderNow() would present an
    // on-screen frame as a side effect.
    if (context.isSnapshot())
        return;

    renderNow();
}

void GPUView::renderNow()
{
    auto* layer = impl->metalLayer.get();

    if (layer.drawableSize.width <= 0)
        return;

    @autoreleasepool
    {
        id<CAMetalDrawable> drawable = [layer nextDrawable];

        if (drawable == nil)
            return;

        auto frame = Frame(Device::shared(),
                           (__bridge void*) drawable,
                           (__bridge void*) impl->msaaTexture.get(),
                           (__bridge void*) impl->depthTexture.get());
        render(frame);
    }
}

Graphics::Image GPUView::renderNativeContent(float scale)
{
    auto bounds = getLocalBounds();
    auto pixelWidth = (NSUInteger) std::lround(bounds.w * scale);
    auto pixelHeight = (NSUInteger) std::lround(bounds.h * scale);

    if (pixelWidth == 0 || pixelHeight == 0)
        return {};

    auto device = (__bridge id<MTLDevice>) Device::shared().nativeDevice();
    auto samples = (NSUInteger) impl->sampleCount;

    @autoreleasepool
    {
        auto makeTarget = [&](MTLPixelFormat format, bool multisample)
        {
            auto descriptor = [MTLTextureDescriptor
                texture2DDescriptorWithPixelFormat:format
                                             width:pixelWidth
                                            height:pixelHeight
                                         mipmapped:NO];
            descriptor.usage = MTLTextureUsageRenderTarget;
            descriptor.storageMode = MTLStorageModePrivate;

            if (multisample)
            {
                descriptor.textureType = MTLTextureType2DMultisample;
                descriptor.sampleCount = samples;
            }

            return [device newTextureWithDescriptor:descriptor];
        };

        auto colorTexture = makeTarget(MTLPixelFormatBGRA8Unorm, false);
        auto msaaTexture =
            samples > 1 ? makeTarget(MTLPixelFormatBGRA8Unorm, true) : nil;
        auto depthTexture =
            impl->depthEnabled ? makeTarget(MTLPixelFormatDepth32Float, samples > 1)
                               : nil;

        {
            auto target = OffscreenTarget {(__bridge void*) colorTexture,
                                           (__bridge void*) msaaTexture,
                                           (__bridge void*) depthTexture};
            auto frame = Frame(Device::shared(), target);
            render(frame);
        }

        // Via a shared buffer so the CPU can read the private colour texture on
        // unified and discrete GPUs alike.
        auto rowBytes = (NSUInteger) pixelWidth * 4;
        auto readback = [device newBufferWithLength:rowBytes * pixelHeight
                                            options:MTLResourceStorageModeShared];

        auto queue = (__bridge id<MTLCommandQueue>) Device::shared().nativeQueue();
        auto commandBuffer = [queue commandBuffer];
        auto blit = [commandBuffer blitCommandEncoder];
        [blit copyFromTexture:colorTexture
                     sourceSlice:0
                     sourceLevel:0
                    sourceOrigin:MTLOriginMake(0, 0, 0)
                      sourceSize:MTLSizeMake(pixelWidth, pixelHeight, 1)
                        toBuffer:readback
               destinationOffset:0
          destinationBytesPerRow:rowBytes
        destinationBytesPerImage:rowBytes * pixelHeight];
        [blit endEncoding];
        [commandBuffer commit];
        [commandBuffer waitUntilCompleted];

        auto image = Graphics::Image {};
        auto* dst = image.prepareForOverwrite((int) pixelWidth, (int) pixelHeight);
        if (dst == nullptr)
            return {};

        // Premultiplied BGRA8, as Core Animation composites it, to the
        // straight-alpha RGBA Image holds.
        auto* src = (const std::uint8_t*) readback.contents;
        auto count = (std::size_t) pixelWidth * pixelHeight;
        for (std::size_t i = 0; i < count; ++i)
        {
            auto b = src[i * 4 + 0];
            auto g = src[i * 4 + 1];
            auto r = src[i * 4 + 2];
            auto a = src[i * 4 + 3];

            if (a == 0)
            {
                dst[i * 4 + 0] = dst[i * 4 + 1] = dst[i * 4 + 2] = dst[i * 4 + 3] = 0;
                continue;
            }

            auto straight = [&](std::uint8_t c) -> std::uint8_t
            { return (std::uint8_t) std::min(255, (c * 255 + a / 2) / a); };

            dst[i * 4 + 0] = straight(r);
            dst[i * 4 + 1] = straight(g);
            dst[i * 4 + 2] = straight(b);
            dst[i * 4 + 3] = a;
        }

        return image;
    }
}

bool GPUView::renderNativeContentToTarget(void* nativeTarget, float)
{
    auto pixelBuffer = (CVPixelBufferRef) nativeTarget;
    if (pixelBuffer == nullptr)
        return false;

    auto surface = CVPixelBufferGetIOSurface(pixelBuffer);
    if (surface == nullptr)
        return false; // not an IOSurface-backed (Metal-compatible) buffer

    auto pixelWidth = CVPixelBufferGetWidth(pixelBuffer);
    auto pixelHeight = CVPixelBufferGetHeight(pixelBuffer);
    if (pixelWidth == 0 || pixelHeight == 0)
        return false;

    auto device = (__bridge id<MTLDevice>) Device::shared().nativeDevice();
    auto samples = (NSUInteger) impl->sampleCount;

    @autoreleasepool
    {
        // Aliases the CVPixelBuffer's IOSurface, so render() writes straight
        // into the buffer the encoder reads.
        auto colorDescriptor = [MTLTextureDescriptor
            texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
                                         width:pixelWidth
                                        height:pixelHeight
                                     mipmapped:NO];
        colorDescriptor.usage = MTLTextureUsageRenderTarget;
        colorDescriptor.storageMode = MTLStorageModeShared;

        auto colorTexture = [device newTextureWithDescriptor:colorDescriptor
                                                   iosurface:surface
                                                       plane:0];
        if (colorTexture == nil)
            return false;

        auto makeTarget = [&](MTLPixelFormat format, bool multisample)
        {
            auto descriptor = [MTLTextureDescriptor
                texture2DDescriptorWithPixelFormat:format
                                             width:pixelWidth
                                            height:pixelHeight
                                         mipmapped:NO];
            descriptor.usage = MTLTextureUsageRenderTarget;
            descriptor.storageMode = MTLStorageModePrivate;

            if (multisample)
            {
                descriptor.textureType = MTLTextureType2DMultisample;
                descriptor.sampleCount = samples;
            }

            return [device newTextureWithDescriptor:descriptor];
        };

        auto msaaTexture =
            samples > 1 ? makeTarget(MTLPixelFormatBGRA8Unorm, true) : nil;
        auto depthTexture =
            impl->depthEnabled ? makeTarget(MTLPixelFormatDepth32Float, samples > 1)
                               : nil;

        auto target = OffscreenTarget {(__bridge void*) colorTexture,
                                       (__bridge void*) msaaTexture,
                                       (__bridge void*) depthTexture};
        auto frame = Frame(Device::shared(), target);
        render(frame);
    }

    return true;
}
} // namespace eacp::GPU
