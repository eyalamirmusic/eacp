
#import <UIKit/UIKit.h>
#include "View.h"
#include "../Graphics/GraphicsContextImpl.h"
#include "../Image/Image.h"

#include <eacp/Core/Threads/Async.h>

#include <map>

@interface NativeView : UIView
{
@public
    eacp::Graphics::View* cppView;

    // Root view only.
    std::map<UITouch*, int> touchIds;
    int nextTouchId;
}
@end

@implementation NativeView

+ (Class)layerClass
{
    return [CALayer class];
}

- (instancetype)initWithFrame:(CGRect)frame
{
    self = [super initWithFrame:frame];
    if (self)
    {
        self.multipleTouchEnabled = YES;
        self.userInteractionEnabled = YES;

        // Transparent, as on macOS, so a view over a GPUView doesn't paint black.
        self.opaque = NO;
        self.backgroundColor = UIColor.clearColor;
        self.layer.opaque = NO;

        nextTouchId = 1;
    }
    return self;
}

- (void)drawLayer:(CALayer*)layer inContext:(CGContextRef)ctx
{
    auto nativeContext = eacp::Graphics::MacOSContext(ctx);
    cppView->paint(nativeContext);
}

- (void)layoutSubviews
{
    [super layoutSubviews];
    [self reportSafeArea];
    cppView->resizeStarted();
    cppView->resized();
    cppView->resizeFinished();
}

- (void)safeAreaInsetsDidChange
{
    [super safeAreaInsetsDidChange];
    [self reportSafeArea];
}

- (void)reportSafeArea
{
    if (cppView->getParent() != nullptr)
        return;

    auto insets = self.safeAreaInsets;
    cppView->setSafeAreaInsets({.top = (float) insets.top,
                                .left = (float) insets.left,
                                .bottom = (float) insets.bottom,
                                .right = (float) insets.right});
}

- (void)setFrame:(CGRect)newFrame
{
    CGRect oldFrame = self.frame;
    [super setFrame:newFrame];

    if (!CGSizeEqualToSize(oldFrame.size, newFrame.size))
    {
        [self setNeedsDisplay];
        [self.layer setNeedsDisplay];
    }
}

- (NativeView*)rootView
{
    NativeView* root = self;
    UIView* current = self.superview;

    while (current != nil)
    {
        if ([current isKindOfClass:[NativeView class]])
            root = (NativeView*) current;

        current = current.superview;
    }

    return root;
}

- (void)dispatchTouches:(NSSet<UITouch*>*)touches
                  phase:(eacp::Graphics::TouchPhase)phase
{
    using eacp::Graphics::TouchPhase;

    auto root = [self rootView];

    for (UITouch* touch in touches)
    {
        auto event = eacp::Graphics::TouchEvent {};

        if (phase == TouchPhase::Began)
        {
            event.id = root->nextTouchId++;
            root->touchIds[touch] = event.id;
        }
        else
        {
            auto found = root->touchIds.find(touch);

            if (found == root->touchIds.end())
                continue;

            event.id = found->second;

            if (phase != TouchPhase::Moved)
                root->touchIds.erase(found);
        }

        auto position = [touch locationInView:root];
        event.pos = {(float) position.x, (float) position.y};
        event.phase = phase;
        event.pressure = (float) touch.force;
        event.tapCount = (int) touch.tapCount;
        event.timestamp = touch.timestamp;

        root->cppView->dispatchTouchEvent(event);
    }
}

- (void)touchesBegan:(NSSet<UITouch*>*)touches withEvent:(UIEvent*)event
{
    [self dispatchTouches:touches phase:eacp::Graphics::TouchPhase::Began];
}

- (void)touchesMoved:(NSSet<UITouch*>*)touches withEvent:(UIEvent*)event
{
    [self dispatchTouches:touches phase:eacp::Graphics::TouchPhase::Moved];
}

- (void)touchesEnded:(NSSet<UITouch*>*)touches withEvent:(UIEvent*)event
{
    [self dispatchTouches:touches phase:eacp::Graphics::TouchPhase::Ended];
}

- (void)touchesCancelled:(NSSet<UITouch*>*)touches withEvent:(UIEvent*)event
{
    [self dispatchTouches:touches phase:eacp::Graphics::TouchPhase::Cancelled];
}

@end

namespace eacp::Graphics
{

NativeView* createNativeView(View* view)
{
    auto rect = CGRectMake(0.f, 0.f, 100.f, 100.f);
    auto newView = [[NativeView alloc] initWithFrame:rect];

    newView.contentScaleFactor = [UIScreen mainScreen].scale;
    newView.layer.contentsScale = [UIScreen mainScreen].scale;
    newView.layer.delegate = newView;

    newView->cppView = view;
    return newView;
}

struct View::Native
{
    Native(View& view) { nativeView = createNativeView(&view); }

    void repaint() { [nativeView.get() setNeedsDisplay]; }

    void setOpacity(float opacity) { nativeView.get().alpha = opacity; }

    // UIKit propagates this to the subtree, nested WKWebView included, the same
    // way AppKit does on macOS.
    void setVisible(bool visible) { nativeView.get().hidden = !visible; }

    Rect getBounds() const { return toRect([nativeView.get() frame]); }
    void setBounds(const Rect& bounds)
    {
        auto frame = toCGRect(bounds);
        [nativeView.get() setFrame:frame];
    }

    float backingScale() const
    {
        return (float) nativeView.get().contentScaleFactor;
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
        // On iOS, we don't have a persistent mouse position
        // Return the last known touch position or center of view
        return {0.f, 0.f};
    }

    void focus() { [nativeView.get() becomeFirstResponder]; }

    bool hasFocus() const { return [nativeView.get() isFirstResponder]; }

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

void View::setOpacity(float opacityToUse)
{
    opacity = opacityToUse;
    impl->setOpacity(opacityToUse);
}

void View::setVisible(bool shouldBeVisible)
{
    if (visible == shouldBeVisible)
        return;

    visible = shouldBeVisible;
    impl->setVisible(shouldBeVisible);
    notifyVisibilityChanged(shouldBeVisible);
}

Rect View::getBounds() const
{
    return impl->getBounds();
}

Image View::renderToImage(float scale)
{
    auto resolvedScale = scale > 0.0f ? scale : impl->backingScale();
    return renderLayerToImage(*this, getLocalBounds(), resolvedScale);
}

Threads::Async<Image> View::renderToImageAsync(float scale)
{
    auto resolvedScale = scale > 0.0f ? scale : impl->backingScale();
    return renderViewToImageAsync(*this, getLocalBounds(), resolvedScale);
}

Point View::getMousePosition() const
{
    return impl->getMousePosition();
}

// Stored and never shown: iOS draws no pointer for a touch. Kept rather than
// ignored so portable code can set a shape unconditionally, and so an iPad with
// a trackpad has one obvious place to grow a real implementation.
void View::setMouseCursor(MouseCursor cursor)
{
    currentCursor = cursor;
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

void View::viewRemoved(View& view )
{
    impl->removeSubview(view);
}

void* View::getNativeLayer()
{
    return impl->getLayer();
}
} // namespace eacp::Graphics
