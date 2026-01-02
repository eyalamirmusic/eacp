
#import <Cocoa/Cocoa.h>
#include "View.h"
#include "MacGraphicsContext.h"
#include "../ObjC/ObjC.h"

namespace eacp::Graphics
{
} // namespace eacp::Graphics

@interface NativeView : NSView
{
@public
    eacp::Graphics::View* cppView;
}
@end

@implementation NativeView

- (void)drawRect:(NSRect)dirtyRect
{
    CGContextRef ctx = (CGContextRef)[[NSGraphicsContext currentContext] CGContext];
    auto nativeContext= eacp::Graphics::MacOSContext(ctx);
    cppView->paint(nativeContext);
}

- (BOOL)isFlipped { return YES; }

- (void)mouseDown:(NSEvent*)event
{
    auto p = [self convertPoint:[event locationInWindow] fromView:nil];

    auto e = eacp::Graphics::MouseEvent();
    e.pos = {(float) p.x, (float) p.y};

    cppView->mouseDown(e);
}

@end

namespace eacp::Graphics
{
NativeView* createNativeView(View* view)
{
    auto rect = NSMakeRect(0.f, 0.f, 100.f, 100.f);
    auto newView = [[NativeView alloc] initWithFrame:rect];
    newView.wantsLayer = YES;
    newView.canDrawConcurrently = YES;
    newView.layerContentsRedrawPolicy = NSViewLayerContentsRedrawOnSetNeedsDisplay;
    newView->cppView = view;
    return newView;
}

struct View::Native
{
    Native(View* view) { nativeView = createNativeView(view); }
    void repaint() { [nativeView.get() setNeedsDisplay:YES]; }

    Rect getBounds() const { return toRect([nativeView.get() bounds]); }

    ObjC::Ptr<NativeView> nativeView;
};

View::View()
    : impl(this)
{
}

void* View::getHandle()
{
    return impl->nativeView.get();
}

void View::repaint()
{
    impl->repaint();
}

Rect View::getBounds() const
{
    return impl->getBounds();
}
} // namespace eacp::Graphics