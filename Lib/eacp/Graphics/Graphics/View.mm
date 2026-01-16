
#import <Cocoa/Cocoa.h>
#include "View.h"
#include "MacGraphicsContext.h"
#include <eacp/Core/Utils/Vectors.h>
#include <ranges>

namespace eacp::Graphics
{
ModifierKeys modifierKeysFromEvent(NSEvent* event)
{
    auto flags = event.modifierFlags;

    return {.shift = (flags & NSEventModifierFlagShift) != 0,
            .control = (flags & NSEventModifierFlagControl) != 0,
            .alt = (flags & NSEventModifierFlagOption) != 0,
            .command = (flags & NSEventModifierFlagCommand) != 0};
}

KeyEvent keyEventFrom(NSEvent* event, KeyEventType type)
{
    auto e = KeyEvent();

    if (event.characters != nil)
        e.characters = [event.characters UTF8String];

    if (event.charactersIgnoringModifiers != nil)
    {
        e.charactersIgnoringModifiers =
            [event.charactersIgnoringModifiers UTF8String];
    }

    e.keyCode = event.keyCode;
    e.type = type;
    e.modifiers = modifierKeysFromEvent(event);
    e.isRepeat = event.isARepeat;
    e.timestamp = event.timestamp;

    return e;
}

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
    e.clickCount = (int) event.clickCount;
    e.pressure = event.pressure;
    e.timestamp = event.timestamp;
    e.delta = {(float) event.deltaX, (float) event.deltaY};

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

// Right mouse button
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

void View::setBounds(const Rect& bounds)
{
    impl->setBounds(bounds);
}

void View::addSubview(View& view)
{
    view.removeFromParent();

    if (Vectors::contains(subviews, &view))
        return;

    view.parent = this;
    subviews.push_back(&view);
    impl->addSubview(view);
}

void View::removeSubview(View& view)
{
    if (Vectors::eraseMatch(subviews, &view))
        impl->removeSubview(view);
}

void View::removeFromParent()
{
    if (parent != nullptr)
        parent->removeSubview(*this);

    parent = nullptr;
}

void View::addLayer(Layer& layer)
{
    if (Vectors::contains(layers, &layer))
        return;

    layers.push_back(&layer);
    layer.attachToLayer(impl->getLayer());
}

void View::removeLayer(Layer& layer)
{
    if (Vectors::eraseMatch(layers, &layer))
        layer.detachFromLayer();
}

void View::resized() {}

Rect View::getLocalBounds() const
{
    auto b = getBounds();
    b.x = 0.f;
    b.y = 0.f;

    return b;
}

void View::addChildren(ChildViews views)
{
    for (auto& view: views)
        addSubview(view);
}

Rect View::getRelativeBounds(const Rect& ratio) const
{
    return getLocalBounds().getRelative(ratio);
}

void View::setBoundsRelative(const Rect& ratio)
{
    if (parent != nullptr)
    {
        setBounds(parent->getRelativeBounds(ratio));
    }
}

void View::scaleToFit()
{
    setBoundsRelative({0.f, 0.f, 1.f, 1.f});
}

void View::scaleToFit(ChildViews views)
{
    for (auto& view: views)
        view.get().scaleToFit();
}

View* View::hitTest(const Point& point)
{
    if (!getLocalBounds().contains(point))
        return nullptr;

    for (auto child: std::ranges::reverse_view(subviews))
    {
        auto childBounds = child->getBounds();
        auto childPoint = Point {point.x - childBounds.x, point.y - childBounds.y};

        if (auto* hit = child->hitTest(childPoint))
            return hit;
    }

    if (properties.handlesMouseEvents)
        return this;

    return nullptr;
}

Point View::convertPointToDescendant(const Point& point, View* descendant)
{
    if (descendant == nullptr || descendant == this)
        return point;

    auto offset = Point(0.f, 0.f);

    auto current = descendant;

    while (current != nullptr && current != this)
    {
        offset = point + offset;
        current = current->parent;
    }

    return point - offset;
}

MouseEvent View::createLocalEvent(const MouseEvent& event,
                                  View* target,
                                  MouseEventType type)
{
    MouseEvent localEvent = event;
    localEvent.pos = convertPointToDescendant(event.pos, target);
    localEvent.downPos = convertPointToDescendant(event.downPos, target);
    localEvent.type = type;
    return localEvent;
}

void View::dispatchMouseEvent(const MouseEvent& event)
{
    auto* target = hitTest(event.pos);

    if (event.type == MouseEventType::Moved || event.type == MouseEventType::Entered)
    {
        if (target != hoveredView)
        {
            if (hoveredView != nullptr)
            {
                hoveredView->handleMouseEvent(
                    createLocalEvent(event, hoveredView, MouseEventType::Exited));
            }

            if (target != nullptr)
            {
                target->handleMouseEvent(
                    createLocalEvent(event, target, MouseEventType::Entered));
            }

            hoveredView = target;
        }

        if (target != nullptr && event.type == MouseEventType::Moved)
        {
            target->handleMouseEvent(
                createLocalEvent(event, target, MouseEventType::Moved));
        }
    }
    else if (event.type == MouseEventType::Exited)
    {
        if (hoveredView != nullptr)
        {
            hoveredView->handleMouseEvent(
                createLocalEvent(event, hoveredView, MouseEventType::Exited));
            hoveredView = nullptr;
        }
    }
    else if (target != nullptr)
    {
        target->handleMouseEvent(createLocalEvent(event, target, event.type));
    }
}

void View::handleMouseEvent(const MouseEvent& event)
{
    switch (event.type)
    {
        case MouseEventType::Down:
            mouseDown(event);
            break;
        case MouseEventType::Up:
            mouseUp(event);
            break;
        case MouseEventType::Dragged:
            mouseDragged(event);
            break;
        case MouseEventType::Moved:
            mouseMoved(event);
            break;
        case MouseEventType::Entered:
            mouseEntered(event);
            break;
        case MouseEventType::Exited:
            mouseExited(event);
            break;
    }
}

bool View::isHovering() const
{
    return getLocalBounds().contains(getMousePosition());
}
} // namespace eacp::Graphics