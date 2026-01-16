
#import <Cocoa/Cocoa.h>
#include "View.h"
#include "ShapeLayer.h"
#include "TextLayer.h"
#include "MacGraphicsContext.h"
#include <eacp/Core/Utils/Vectors.h>

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

    // If the frame size changed, we need to redraw
    if (!NSEqualSizes(oldFrame.size, newFrame.size))
    {
        [self setNeedsDisplay:YES];
        // For layer-backed views, also invalidate the layer
        if (self.wantsLayer && self.layer)
        {
            [self.layer setNeedsDisplay];
        }
    }
}

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
    newView.layerContentsRedrawPolicy = NSViewLayerContentsRedrawOnSetNeedsDisplay;
    newView.layer.contentsScale = [NSScreen mainScreen].backingScaleFactor;
    newView.layer.delegate = newView; // Explicitly set delegate

    newView->cppView = view;
    return newView;
}
struct View::Native
{
    Native(View& view) { nativeView = createNativeView(&view); }

    void repaint() { [nativeView.get() setNeedsDisplay:YES]; }

    Rect getBounds() const { return toRect([nativeView.get() bounds]); }
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
} // namespace eacp::Graphics