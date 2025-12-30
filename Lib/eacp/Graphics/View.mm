
#import <Cocoa/Cocoa.h>
#include "View.h"
#include "MacGraphicsContext.h"
#include "../ObjC/ObjC.h"

using namespace eacp::Graphics;

@interface NativeView : NSView
{
    View* cppView;
}
- (instancetype)initWithCppView:(View*)cppView frame:(NSRect)frame;
@end

@implementation NativeView

- (instancetype)initWithCppView:(View*)viewToUse frame:(NSRect)frame
{
    self = [super initWithFrame:frame];

    if (self)
        cppView = viewToUse;

    return self;
}

- (void)drawRect:(NSRect)dirtyRect
{
    [super drawRect:dirtyRect];

    auto cgContext = [[NSGraphicsContext currentContext] CGContext];

    CGContextSaveGState(cgContext);

    CGContextTranslateCTM(cgContext, 0, self.bounds.size.height);
    CGContextScaleCTM(cgContext, 1.0, -1.0);

    auto ctxWrapper = MacOSContext(cgContext);

    cppView->paint(ctxWrapper);

    CGContextRestoreGState(cgContext);
}

- (void)mouseDown:(NSEvent*)event
{
    auto p = [self convertPoint:[event locationInWindow] fromView:nil];
    auto flippedY = self.bounds.size.height - p.y;

    auto e = MouseEvent();
    e.pos = {(float) p.x, (float) flippedY};

    cppView->mouseDown(e);
}

@end

namespace eacp::Graphics
{
struct View::Impl
{
    Impl(View* view)
    {
        auto rect = NSMakeRect(0.f, 0.f, 100.f, 100.f);
        nativeView = [[NativeView alloc] initWithCppView:view frame:rect];
    }

    void repaint() { [nativeView.get() setNeedsDisplay:YES]; }

    ObjC::Ptr<NativeView> nativeView;
};

View::View()
{
    impl = std::make_shared<Impl>(this);
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