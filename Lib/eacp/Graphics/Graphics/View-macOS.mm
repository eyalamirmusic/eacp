
#import <Cocoa/Cocoa.h>
#include "View.h"
#include "GraphicsContextImpl.h"
#include "Keyboard-MacOS.h"

namespace eacp::Graphics
{

} // namespace eacp::Graphics

@interface NativeView : NSView <CALayerDelegate>
{
@public
    eacp::Graphics::View* cppView;
    NSPoint mouseDownPosition;
}
@end

@implementation NativeView

- (void)drawRect:(NSRect)dirtyRect
{
}

- (void)drawLayer:(CALayer*)layer inContext:(CGContextRef)ctx
{
    auto nativeContext = eacp::Graphics::MacOSContext(ctx);
    cppView->paint(nativeContext);
}

- (void)layout
{
    [super layout];
    cppView->resized();
}

- (BOOL)isFlipped
{
    return YES;
}

- (BOOL)isOpaque
{
    return NO;
}

- (BOOL)acceptsFirstResponder
{
    return YES;
}

- (void)viewDidChangeBackingProperties
{
    [super viewDidChangeBackingProperties];
    self.layer.contentsScale = self.window.backingScaleFactor;
}

- (void)setFrame:(NSRect)newFrame
{
    NSRect oldFrame = [self frame];
    [super setFrame:newFrame];

    if (!NSEqualSizes(oldFrame.size, newFrame.size))
    {
        [self setNeedsDisplay:YES];

        if (self.wantsLayer && self.layer)
        {
            [self.layer setNeedsDisplay];
        }
    }
}

- (NativeView*)rootView
{
    NativeView* root = self;
    NSView* current = self.superview;

    while (current != nil)
    {
        if ([current isKindOfClass:[NativeView class]])
            root = (NativeView*) current;

        current = current.superview;
    }

    return root;
}

- (eacp::Graphics::MouseButton)mouseButtonFromEvent:(NSEvent*)event
{
    switch (event.buttonNumber)
    {
        case 0:
            return eacp::Graphics::MouseButton::Left;
        case 1:
            return eacp::Graphics::MouseButton::Right;
        case 2:
            return eacp::Graphics::MouseButton::Middle;
        default:
            return eacp::Graphics::MouseButton::Other;
    }
}

- (void)dispatchMouseEvent:(NSEvent*)event type:(eacp::Graphics::MouseEventType)type
{
    auto root = [self rootView];
    auto windowPos = [event locationInWindow];
    auto localPos = [root convertPoint:windowPos fromView:nil];

    auto e = eacp::Graphics::MouseEvent();

    e.pos = {(float) localPos.x, (float) localPos.y};
    e.type = type;
    e.button = [self mouseButtonFromEvent:event];
    e.modifiers = eacp::Graphics::modifierKeysFromEvent(event);

    e.timestamp = event.timestamp;
    e.delta = {(float) event.deltaX, (float) event.deltaY};

    if (type == eacp::Graphics::MouseEventType::Down
        || type == eacp::Graphics::MouseEventType::Up)
    {
        e.pressure = event.pressure;
        e.clickCount = (int) event.clickCount;
    }

    if (type == eacp::Graphics::MouseEventType::Down)
        root->mouseDownPosition = localPos;

    e.downPos = {(float) root->mouseDownPosition.x,
                 (float) root->mouseDownPosition.y};

    root->cppView->dispatchMouseEvent(e);
}

- (void)mouseDown:(NSEvent*)event
{
    [self dispatchMouseEvent:event type:eacp::Graphics::MouseEventType::Down];
}

- (void)mouseUp:(NSEvent*)event
{
    [self dispatchMouseEvent:event type:eacp::Graphics::MouseEventType::Up];
}

- (void)mouseDragged:(NSEvent*)event
{
    [self dispatchMouseEvent:event type:eacp::Graphics::MouseEventType::Dragged];
}

- (void)mouseMoved:(NSEvent*)event
{
    [self dispatchMouseEvent:event type:eacp::Graphics::MouseEventType::Moved];
}

- (void)mouseEntered:(NSEvent*)event
{
    [self dispatchMouseEvent:event type:eacp::Graphics::MouseEventType::Entered];
}

- (void)mouseExited:(NSEvent*)event
{
    [self dispatchMouseEvent:event type:eacp::Graphics::MouseEventType::Exited];
}

- (void)rightMouseDown:(NSEvent*)event
{
    [self dispatchMouseEvent:event type:eacp::Graphics::MouseEventType::Down];
}

- (void)rightMouseUp:(NSEvent*)event
{
    [self dispatchMouseEvent:event type:eacp::Graphics::MouseEventType::Up];
}

- (void)rightMouseDragged:(NSEvent*)event
{
    [self dispatchMouseEvent:event type:eacp::Graphics::MouseEventType::Dragged];
}

// Other mouse buttons (middle, etc.)
- (void)otherMouseDown:(NSEvent*)event
{
    [self dispatchMouseEvent:event type:eacp::Graphics::MouseEventType::Down];
}

- (void)otherMouseUp:(NSEvent*)event
{
    [self dispatchMouseEvent:event type:eacp::Graphics::MouseEventType::Up];
}

- (void)otherMouseDragged:(NSEvent*)event
{
    [self dispatchMouseEvent:event type:eacp::Graphics::MouseEventType::Dragged];
}

- (void)keyDown:(NSEvent*)event
{
    auto e = eacp::Graphics::keyEventFrom(event, eacp::Graphics::KeyEventType::Down);
    cppView->keyDown(e);
}

- (void)keyUp:(NSEvent*)event
{
    auto e = eacp::Graphics::keyEventFrom(event, eacp::Graphics::KeyEventType::Up);
    cppView->keyUp(e);
}

- (void)updateTrackingAreas
{
    [super updateTrackingAreas];

    for (NSTrackingArea* area in self.trackingAreas)
        [self removeTrackingArea:area];

    NSTrackingAreaOptions options =
        NSTrackingMouseEnteredAndExited | NSTrackingMouseMoved
        | NSTrackingActiveInKeyWindow | NSTrackingInVisibleRect;

    NSTrackingArea* trackingArea = [[NSTrackingArea alloc] initWithRect:self.bounds
                                                                options:options
                                                                  owner:self
                                                               userInfo:nil];
    [self addTrackingArea:trackingArea];
}

@end
namespace eacp::Graphics
{
NativeView* createNativeView(View* view)
{
    auto rect = NSMakeRect(0.f, 0.f, 100.f, 100.f);
    auto newView = [[NativeView alloc] initWithFrame:rect];

    newView.wantsLayer = YES;
    newView.layerContentsRedrawPolicy = NSViewLayerContentsRedrawOnSetNeedsDisplay;
    newView.layer.contentsScale = [NSScreen mainScreen].backingScaleFactor;
    newView.layer.delegate = newView;

    newView->cppView = view;
    return newView;
}

struct View::Native
{
    Native(View& view) { nativeView = createNativeView(&view); }

    void repaint() { [nativeView.get() setNeedsDisplay:YES]; }

    Rect getBounds() const { return toRect([nativeView.get() frame]); }
    void setBounds(const Rect& bounds)
    {
        auto frame = toCGRect(bounds);
        [nativeView.get() setFrame:frame];
    }

    void addSubview(View& view)
    {
        auto* childNativeView = (NativeView*) view.getHandle();
        [nativeView.get() addSubview:childNativeView];
    }

    void removeSubview(View& view)
    {
        auto* childNativeView = (NativeView*) view.getHandle();
        [childNativeView removeFromSuperview];
    }

    CALayer* getLayer() { return nativeView.get().layer; }

    Point getMousePosition() const
    {
        auto view = nativeView.get();
        auto windowPoint = [view.window mouseLocationOutsideOfEventStream];
        auto localPoint = [view convertPoint:windowPoint fromView:nil];

        return {(float) localPoint.x, (float) localPoint.y};
    }

    void focus()
    {
        auto view = nativeView.get();
        [view.window makeFirstResponder:view];
    }

    bool hasFocus() const
    {
        auto view = nativeView.get();
        return view.window.firstResponder == view;
    }

    ObjC::Ptr<NativeView> nativeView;
};

View::View()
    : impl(*this)
{
}

View::~View()
{
    for (auto* layer: layers)
        layer->detachFromLayer();

    removeFromParent();
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

Point View::getMousePosition() const
{
    return impl->getMousePosition();
}

void View::focus()
{
    impl->focus();
}

bool View::hasFocus() const
{
    return impl->hasFocus();
}

void View::setBounds(const Rect& bounds)
{
    impl->setBounds(bounds);
}

void View::addSubview(View& view)
{
    if (prepareAddSubview(view))
        impl->addSubview(view);
}

void View::removeSubview(View& view)
{
    if (prepareRemoveSubview(view))
        impl->removeSubview(view);

    if (hoveredView == &view)
        hoveredView = nullptr;
}

void* View::getNativeLayer()
{
    return impl->getLayer();
}

void View::addLayer(Layer& layer)
{
    if (prepareAddLayer(layer))
        layer.attachToLayer(impl->getLayer());
}

void View::removeLayer(Layer& layer)
{
    prepareRemoveLayer(layer);
}

} // namespace eacp::Graphics
