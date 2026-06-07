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

        metalLayer.get().frame = bounds;
        metalLayer.get().contentsScale = scale;
        metalLayer.get().drawableSize =
            CGSizeMake(bounds.size.width * scale, bounds.size.height * scale);
    }

    void ensureRenderLoop()
    {
        if (displayLink == nullptr)
            displayLink =
                makeOwned<Threads::DisplayLink>([this] { view.renderTick(); });
    }

    GPUView& view;
    ObjC::Ptr<CAMetalLayer> metalLayer;
    OwningPointer<Threads::DisplayLink> displayLink;
};

GPUView::GPUView()
    : impl(*this)
{
}

GPUView::~GPUView() = default;

void GPUView::resized()
{
    Graphics::View::resized();
    impl->updateSize();
    impl->ensureRenderLoop();
}

void GPUView::renderTick()
{
    auto* layer = impl->metalLayer.get();

    if (layer.drawableSize.width <= 0)
        return;

    @autoreleasepool
    {
        id<CAMetalDrawable> drawable = [layer nextDrawable];

        if (drawable == nil)
            return;

        auto frame = Frame(Device::shared(), (__bridge void*) drawable);
        render(frame);
    }
}
} // namespace eacp::GPU
