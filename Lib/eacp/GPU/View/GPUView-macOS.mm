#import <Cocoa/Cocoa.h>
#import <Metal/Metal.h>
#import <QuartzCore/CAMetalLayer.h>

#include "GPUView.h"

#include "../Device/Device.h"
#include "../Frame/Frame.h"

#include <eacp/Core/ObjC/ObjC.h>
#include <eacp/Core/Utils/Containers.h>
#include <eacp/Graphics/Helpers/DisplayLink.h>
#include <eacp/Graphics/Primitives/GraphicUtils.h>

// Suppresses implicit Core Animation actions so the layer tracks the view's
// size instantly during live resize instead of animating to it (matches the
// Immediate* layer convention in eacp::Graphics).
@interface ImmediateMetalLayer : CAMetalLayer
@end

@implementation ImmediateMetalLayer

- (id<CAAction>)actionForKey:(NSString*)event
{
    return [NSNull null];
}

@end

namespace eacp::GPU
{
struct GPUView::Native
{
    explicit Native(GPUView& viewToUse)
        : view(viewToUse)
    {
        metalLayer = [[ImmediateMetalLayer alloc] init];

        auto metalDevice = (__bridge id<MTLDevice>) Device::shared().nativeDevice();
        metalLayer.get().device = metalDevice;
        metalLayer.get().pixelFormat = MTLPixelFormatBGRA8Unorm;
        metalLayer.get().framebufferOnly = YES;

        // Present the drawable inside the current CATransaction so new content
        // lands atomically with the layer's new size during live resize, rather
        // than the old drawable being stretched until the next async present.
        metalLayer.get().presentsWithTransaction = YES;

        auto base = (__bridge CALayer*) view.getNativeLayer();
        [base addSublayer:metalLayer.get()];
    }

    ~Native() { [metalLayer.get() removeFromSuperlayer]; }

    void updateSize()
    {
        auto* nativeView = (__bridge NSView*) view.getHandle();
        auto scale = nativeView.window != nil
                         ? nativeView.window.backingScaleFactor
                         : NSScreen.mainScreen.backingScaleFactor;

        auto bounds = Graphics::toCGRect(view.getLocalBounds());
        auto pixelWidth = (NSUInteger) (bounds.size.width * scale);
        auto pixelHeight = (NSUInteger) (bounds.size.height * scale);

        metalLayer.get().frame = bounds;
        metalLayer.get().contentsScale = scale;
        metalLayer.get().drawableSize = CGSizeMake(pixelWidth, pixelHeight);

        updateMultisampleTexture(pixelWidth, pixelHeight);
        updateDepthTexture(pixelWidth, pixelHeight);
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
            displayLink =
                makeOwned<Threads::DisplayLink>([this] { view.renderNow(); });
    }

    void stopContinuous() { displayLink = nullptr; }

    GPUView& view;
    int sampleCount = 4;
    bool continuous = false;
    bool depthEnabled = false;
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

void GPUView::resized()
{
    Graphics::View::resized();
    impl->updateSize();

    // Draw at the new size now, synchronously within the layout/resize pass,
    // instead of waiting for the async display link a frame or more later.
    renderNow();
}

void GPUView::paint(Graphics::Context&)
{
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
} // namespace eacp::GPU
