
#import <Cocoa/Cocoa.h>
#include "View.h"
#include "MacGraphicsContext.h"
#include <eacp/Core/Utils/Vectors.h>
#include <ranges>

namespace eacp::Graphics
{
} // namespace eacp::Graphics

@interface NativeView : NSView <CALayerDelegate>
{
@public
    eacp::Graphics::View* cppView;
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

- (void)dispatchMouseEvent:(NSEvent*)event type:(eacp::Graphics::MouseEventType)type
{
    NativeView* root = [self rootView];
    auto p = [root convertPoint:[event locationInWindow] fromView:nil];
    auto e = eacp::Graphics::MouseEvent {{(float) p.x, (float) p.y}, type};
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

void View::dispatchMouseEvent(const MouseEvent& event)
{
    auto* target = hitTest(event.pos);

    if (event.type == MouseEventType::Moved || event.type == MouseEventType::Entered)
    {
        if (target != hoveredView)
        {
            if (hoveredView != nullptr)
            {
                auto localPos = convertPointToDescendant(event.pos, hoveredView);
                hoveredView->handleMouseEvent({localPos, MouseEventType::Exited});
            }

            if (target != nullptr)
            {
                auto localPos = convertPointToDescendant(event.pos, target);
                target->handleMouseEvent({localPos, MouseEventType::Entered});
            }

            hoveredView = target;
        }

        if (target != nullptr && event.type == MouseEventType::Moved)
        {
            auto localPos = convertPointToDescendant(event.pos, target);
            target->handleMouseEvent({localPos, MouseEventType::Moved});
        }
    }
    else if (event.type == MouseEventType::Exited)
    {
        if (hoveredView != nullptr)
        {
            auto localPos = convertPointToDescendant(event.pos, hoveredView);
            hoveredView->handleMouseEvent({localPos, MouseEventType::Exited});
            hoveredView = nullptr;
        }
    }
    else if (target != nullptr)
    {
        auto localPos = convertPointToDescendant(event.pos, target);
        target->handleMouseEvent({localPos, event.type});
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