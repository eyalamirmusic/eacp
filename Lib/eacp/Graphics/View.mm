
#import <Cocoa/Cocoa.h>
#include "View.h"
#include "MacGraphicsContext.h"
#include "../ObjC/ObjC.h"

namespace eacp::Graphics
{
void paintNative(View& view, NSRect bounds)
{
    auto cgContext = [[NSGraphicsContext currentContext] CGContext];

    CGContextSaveGState(cgContext);

    CGContextTranslateCTM(cgContext, 0, bounds.size.height);
    CGContextScaleCTM(cgContext, 1.0, -1.0);

    auto ctxWrapper = eacp::Graphics::MacOSContext(cgContext);

    view.paint(ctxWrapper);

    CGContextRestoreGState(cgContext);
}
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
    [super drawRect:dirtyRect];
    eacp::Graphics::paintNative(*cppView, self.bounds);
}

- (void)mouseDown:(NSEvent*)event
{
    auto p = [self convertPoint:[event locationInWindow] fromView:nil];
    auto flippedY = self.bounds.size.height - p.y;

    auto e = eacp::Graphics::MouseEvent();
    e.pos = {(float) p.x, (float) flippedY};

    cppView->mouseDown(e);
}

@end

namespace eacp::Graphics
{
NativeView* createNativeView(View* view)
{
    auto rect = NSMakeRect(0.f, 0.f, 100.f, 100.f);
    auto newView = [[NativeView alloc] initWithFrame:rect];
    newView->cppView = view;
    return newView;
}

struct View::Native
{
    Native(View* view) { nativeView = createNativeView(view); }
    void repaint() { [nativeView.get() setNeedsDisplay:YES]; }

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
} // namespace eacp::Graphics