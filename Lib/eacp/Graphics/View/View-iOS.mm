
#import <UIKit/UIKit.h>
#include "View.h"
#include "GraphicsContextImpl.h"

@interface NativeView : UIView
{
@public
    eacp::Graphics::View* cppView;
    CGPoint touchDownPosition;
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
        self.multipleTouchEnabled = NO;
        self.userInteractionEnabled = YES;
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
    cppView->resized();
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

- (void)dispatchTouchEvent:(UITouch*)touch type:(eacp::Graphics::MouseEventType)type
{
    auto root = [self rootView];
    auto localPos = [touch locationInView:root];

    auto e = eacp::Graphics::MouseEvent();

    e.pos = {(float) localPos.x, (float) localPos.y};
    e.type = type;
    e.button = eacp::Graphics::MouseButton::Left;
    e.modifiers = {};

    e.timestamp = touch.timestamp;
    e.delta = {0.f, 0.f};

    if (type == eacp::Graphics::MouseEventType::Down
        || type == eacp::Graphics::MouseEventType::Up)
    {
        e.pressure = (float) touch.force;
        e.clickCount = (int) touch.tapCount;
    }

    if (type == eacp::Graphics::MouseEventType::Down)
        root->touchDownPosition = localPos;

    e.downPos = {(float) root->touchDownPosition.x,
                 (float) root->touchDownPosition.y};

    root->cppView->dispatchMouseEvent(e);
}

- (void)touchesBegan:(NSSet<UITouch*>*)touches withEvent:(UIEvent*)event
{
    UITouch* touch = [touches anyObject];
    if (touch)
    {
        [self dispatchTouchEvent:touch type:eacp::Graphics::MouseEventType::Down];
    }
}

- (void)touchesMoved:(NSSet<UITouch*>*)touches withEvent:(UIEvent*)event
{
    UITouch* touch = [touches anyObject];
    if (touch)
    {
        [self dispatchTouchEvent:touch type:eacp::Graphics::MouseEventType::Dragged];
    }
}

- (void)touchesEnded:(NSSet<UITouch*>*)touches withEvent:(UIEvent*)event
{
    UITouch* touch = [touches anyObject];
    if (touch)
    {
        [self dispatchTouchEvent:touch type:eacp::Graphics::MouseEventType::Up];
    }
}

- (void)touchesCancelled:(NSSet<UITouch*>*)touches withEvent:(UIEvent*)event
{
    UITouch* touch = [touches anyObject];
    if (touch)
    {
        [self dispatchTouchEvent:touch type:eacp::Graphics::MouseEventType::Up];
    }
}

@end

namespace eacp::Graphics
{

NativeView* createNativeView(View* view)
{
    auto rect = CGRectMake(0.f, 0.f, 100.f, 100.f);
    auto newView = [[NativeView alloc] initWithFrame:rect];

    newView.layer.contentsScale = [UIScreen mainScreen].scale;
    newView.layer.delegate = newView;

    newView->cppView = view;
    return newView;
}

struct View::Native
{
    Native(View& view) { nativeView = createNativeView(&view); }

    void repaint() { [nativeView.get() setNeedsDisplay]; }

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
        // On iOS, we don't have a persistent mouse position
        // Return the last known touch position or center of view
        return {0.f, 0.f};
    }

    void focus()
    {
        [nativeView.get() becomeFirstResponder];
    }

    bool hasFocus() const
    {
        return [nativeView.get() isFirstResponder];
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
