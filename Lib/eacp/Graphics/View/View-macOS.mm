
#import <Cocoa/Cocoa.h>
#include "View.h"
#include "../Graphics/GraphicsContextImpl.h"
#include "../Window/MouseLock-macOS.h"
#include "../Graphics/Keyboard-MacOS.h"

#include <eacp/Core/ObjC/RuntimeClass.h>

namespace eacp::Graphics
{
namespace
{
Class getNativeViewClass();

View* getView(id self)
{
    return (View*) ObjC::getIvar<void*>(self, "cppView");
}

NSView* getRootView(id self)
{
    auto* root = (NSView*) self;
    NSView* current = [root superview];

    while (current != nil)
    {
        if ([current isKindOfClass:getNativeViewClass()])
            root = current;

        current = current.superview;
    }

    return root;
}

MouseButton mouseButtonFromEvent(NSEvent* event)
{
    switch (event.buttonNumber)
    {
        case 0:
            return MouseButton::Left;
        case 1:
            return MouseButton::Right;
        case 2:
            return MouseButton::Middle;
        default:
            return MouseButton::Other;
    }
}

void dispatchMouseEvent(id self, NSEvent* event, MouseEventType type)
{
    auto* root = getRootView(self);
    auto windowPos = [event locationInWindow];
    auto localPos = [root convertPoint:windowPos fromView:nil];

    auto e = MouseEvent();

    e.pos = {(float) localPos.x, (float) localPos.y};
    e.type = type;
    e.button = mouseButtonFromEvent(event);
    e.modifiers = modifierKeysFromEvent(event);

    e.timestamp = event.timestamp;

    // Both figures, always: `delta` is the pointer's movement, shaped by the
    // system's acceleration curve, and `rawDelta` is the device's own. A widget
    // dragged by the pointer wants the first; a camera wants the second. See
    // MouseEvent.
    e.delta = {(float) event.deltaX, (float) event.deltaY};
    e.rawDelta = e.delta;

    if (CGEventRef cgEvent = [event CGEvent])
        e.rawDelta = {(float) CGEventGetIntegerValueField(
                          cgEvent, kCGEventUnacceleratedPointerMovementX),
                      (float) CGEventGetIntegerValueField(
                          cgEvent, kCGEventUnacceleratedPointerMovementY)};

    // A warp is not motion, however the system reports it.
    if (detail::cursorWasWarped)
    {
        e.delta = {};
        e.rawDelta = {};
        detail::cursorWasWarped = false;
    }

    if (type == MouseEventType::Down || type == MouseEventType::Up)
    {
        e.pressure = event.pressure;
        e.clickCount = (int) event.clickCount;
    }

    auto& mouseDownPosition = ObjC::getIvar<NSPoint>(root, "mouseDownPosition");

    if (type == MouseEventType::Down)
        mouseDownPosition = localPos;

    e.downPos = {(float) mouseDownPosition.x, (float) mouseDownPosition.y};

    if (auto* view = getView(root))
        view->dispatchMouseEvent(e);
}

void drawRect(id, SEL, NSRect)
{
}

void drawLayerInContext(id self, SEL, CALayer*, CGContextRef ctx)
{
    if (auto* view = getView(self))
    {
        auto nativeContext = MacOSContext(ctx);
        view->paint(nativeContext);
    }
}

void layout(id self, SEL)
{
    ObjC::sendSuper<void>(self, [NSView class], @selector(layout));

    if (auto* view = getView(self))
        view->resized();
}

BOOL isFlipped(id, SEL)
{
    return YES;
}

BOOL isOpaque(id, SEL)
{
    return NO;
}

BOOL acceptsFirstResponder(id, SEL)
{
    return YES;
}

void viewDidChangeBackingProperties(id self, SEL)
{
    ObjC::sendSuper<void>(
        self, [NSView class], @selector(viewDidChangeBackingProperties));

    auto* view = (NSView*) self;
    view.layer.contentsScale = view.window.backingScaleFactor;
}

void setFrame(id self, SEL, NSRect newFrame)
{
    auto* view = (NSView*) self;
    auto oldFrame = [view frame];

    ObjC::sendSuper<void>(self, [NSView class], @selector(setFrame:), newFrame);

    if (!NSEqualSizes(oldFrame.size, newFrame.size))
    {
        [view setNeedsDisplay:YES];

        if (view.wantsLayer && view.layer)
            [view.layer setNeedsDisplay];
    }
}

void mouseDown(id self, SEL, NSEvent* event)
{
    dispatchMouseEvent(self, event, MouseEventType::Down);
}

void mouseUp(id self, SEL, NSEvent* event)
{
    dispatchMouseEvent(self, event, MouseEventType::Up);
}

void mouseDragged(id self, SEL, NSEvent* event)
{
    dispatchMouseEvent(self, event, MouseEventType::Dragged);
}

void mouseMoved(id self, SEL, NSEvent* event)
{
    dispatchMouseEvent(self, event, MouseEventType::Moved);
}

void mouseEntered(id self, SEL, NSEvent* event)
{
    dispatchMouseEvent(self, event, MouseEventType::Entered);
}

void mouseExited(id self, SEL, NSEvent* event)
{
    dispatchMouseEvent(self, event, MouseEventType::Exited);
}

void keyDown(id self, SEL, NSEvent* event)
{
    if (auto* view = getView(self))
        view->keyDown(keyEventFrom(event, KeyEventType::Down));
}

void keyUp(id self, SEL, NSEvent* event)
{
    if (auto* view = getView(self))
        view->keyUp(keyEventFrom(event, KeyEventType::Up));
}

void updateTrackingAreas(id self, SEL)
{
    ObjC::sendSuper<void>(self, [NSView class], @selector(updateTrackingAreas));

    auto* view = (NSView*) self;

    for (NSTrackingArea* area in view.trackingAreas)
        [view removeTrackingArea:area];

    NSTrackingAreaOptions options =
        NSTrackingMouseEnteredAndExited | NSTrackingMouseMoved
        | NSTrackingActiveInKeyWindow | NSTrackingInVisibleRect;

    NSTrackingArea* trackingArea =
        [[NSTrackingArea alloc] initWithRect:view.bounds
                                     options:options
                                       owner:view
                                    userInfo:nil];
    [view addTrackingArea:trackingArea];
    [trackingArea release];
}

Class getNativeViewClass()
{
    static auto instance = []
    {
        auto builder = new ObjC::RuntimeClass<NSView>("EacpNativeView");

        builder->addIvar<void*>("cppView");
        builder->addIvar<NSPoint>("mouseDownPosition");

        builder->addProtocol(@protocol(CALayerDelegate));

        builder->addMethod(@selector(drawRect:), drawRect);
        builder->addMethod(@selector(drawLayer:inContext:), drawLayerInContext);
        builder->addMethod(@selector(layout), layout);
        builder->addMethod(@selector(isFlipped), isFlipped);
        builder->addMethod(@selector(isOpaque), isOpaque);
        builder->addMethod(@selector(acceptsFirstResponder),
                           acceptsFirstResponder);
        builder->addMethod(@selector(viewDidChangeBackingProperties),
                           viewDidChangeBackingProperties);
        builder->addMethod(@selector(setFrame:), setFrame);

        builder->addMethod(@selector(mouseDown:), mouseDown);
        builder->addMethod(@selector(mouseUp:), mouseUp);
        builder->addMethod(@selector(mouseDragged:), mouseDragged);
        builder->addMethod(@selector(mouseMoved:), mouseMoved);
        builder->addMethod(@selector(mouseEntered:), mouseEntered);
        builder->addMethod(@selector(mouseExited:), mouseExited);
        builder->addMethod(@selector(rightMouseDown:), mouseDown);
        builder->addMethod(@selector(rightMouseUp:), mouseUp);
        builder->addMethod(@selector(rightMouseDragged:), mouseDragged);
        builder->addMethod(@selector(otherMouseDown:), mouseDown);
        builder->addMethod(@selector(otherMouseUp:), mouseUp);
        builder->addMethod(@selector(otherMouseDragged:), mouseDragged);

        builder->addMethod(@selector(keyDown:), keyDown);
        builder->addMethod(@selector(keyUp:), keyUp);
        builder->addMethod(@selector(updateTrackingAreas),
                           updateTrackingAreas);

        builder->registerClass();
        return builder;
    }();

    return instance->get();
}

NSView* createNativeView(View* view)
{
    auto rect = NSMakeRect(0.f, 0.f, 100.f, 100.f);
    NSView* newView = [[getNativeViewClass() alloc] initWithFrame:rect];

    newView.wantsLayer = YES;
    newView.layerContentsRedrawPolicy = NSViewLayerContentsRedrawOnSetNeedsDisplay;
    newView.layer.contentsScale = [NSScreen mainScreen].backingScaleFactor;
    newView.layer.delegate = (id<CALayerDelegate>) newView;

    ObjC::getIvar<void*>(newView, "cppView") = view;
    return newView;
}
} // namespace

struct View::Native
{
    Native(View& view) { nativeView = createNativeView(&view); }

    // AppKit and CoreAnimation can outlive our ObjC::Ptr reference — a pending
    // CA transaction still draws the backing layer after the C++ View died
    // (e.g. a window torn down right after a repaint). Break both back-links so
    // a surviving NativeView is inert instead of dereferencing a freed View.
    ~Native()
    {
        auto* view = nativeView.get();
        ObjC::getIvar<void*>(view, "cppView") = nullptr;
        view.layer.delegate = nil;
        [view removeFromSuperview];
    }

    void repaint() { [nativeView.get() setNeedsDisplay:YES]; }

    void setOpacity(float opacity) { nativeView.get().alphaValue = opacity; }

    Rect getBounds() const { return toRect([nativeView.get() frame]); }
    void setBounds(const Rect& bounds)
    {
        auto frame = toCGRect(bounds);
        [nativeView.get() setFrame:frame];
    }

    void addSubview(View& view)
    {
        auto* childNativeView = (NSView*) view.getHandle();
        [nativeView.get() addSubview:childNativeView];
    }

    void removeSubview(View& view)
    {
        auto* childNativeView = (NSView*) view.getHandle();
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

    ObjC::Ptr<NSView> nativeView;
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

void View::setOpacity(float opacity)
{
    impl->setOpacity(opacity);
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

void View::viewAdded(View& view)
{
    impl->addSubview(view);
}

void View::viewRemoved(View& view)
{
    impl->removeSubview(view);
}

void* View::getNativeLayer()
{
    return impl->getLayer();
}
} // namespace eacp::Graphics
