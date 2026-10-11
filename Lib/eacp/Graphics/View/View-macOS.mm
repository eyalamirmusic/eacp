
#import <Cocoa/Cocoa.h>
#include "View.h"
#include "FileDragOut-macOS.h"
#include "../Graphics/GraphicsContextImpl.h"
#include "../Image/Image.h"
#include "../Window/MouseLock-macOS.h"

#include <eacp/Core/Threads/Async.h>
#include "../Graphics/Keyboard-MacOS.h"

#include <eacp/Core/ObjC/RuntimeClass.h>

#include <cstring>
#include <memory>

namespace eacp::Graphics
{
namespace
{
Class getNativeViewClass();

View* getView(id self)
{
    return (View*) ObjC::getIvar<void*>(self, "cppView");
}

bool ignoresMouseEvents(id self)
{
    auto* view = getView(self);
    return view != nullptr && view->getProperties().ignoresMouseEvents;
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

// The screen is the one frame of reference a drag can be measured against. A
// view's own space is not: it can move under a stationary pointer — a scrolled
// container, or an editor a plugin host is resizing around the very drag that
// asked for it — and a start point captured in that space and then reused would
// report the view's movement as pointer travel that never happened.
NSPoint toScreen(NSView* view, NSPoint windowPoint)
{
    NSWindow* window = view.window;
    return window != nil ? [window convertPointToScreen:windowPoint] : windowPoint;
}

NSPoint fromScreen(NSView* view, NSPoint screenPoint)
{
    NSWindow* window = view.window;
    return window != nil ? [window convertPointFromScreen:screenPoint] : screenPoint;
}

NSEvent* mouseEventInDispatch = nil;

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

    auto& mouseDownScreenPosition =
        ObjC::getIvar<NSPoint>(root, "mouseDownScreenPosition");

    if (type == MouseEventType::Down)
        mouseDownScreenPosition = toScreen(root, windowPos);

    auto downPos =
        [root convertPoint:fromScreen(root, mouseDownScreenPosition) fromView:nil];

    e.downPos = {(float) downPos.x, (float) downPos.y};

    if (auto* view = getView(root))
    {
        auto* previousEvent = mouseEventInDispatch;
        mouseEventInDispatch = event;
        view->dispatchMouseEvent(e);
        mouseEventInDispatch = previousEvent;
    }
}

ScrollPhase gesturePhaseFromEvent(NSEvent* event)
{
    switch (event.phase)
    {
        case NSEventPhaseBegan:
        case NSEventPhaseMayBegin:
            return ScrollPhase::Began;
        case NSEventPhaseChanged:
            return ScrollPhase::Changed;
        case NSEventPhaseEnded:
        case NSEventPhaseCancelled:
            return ScrollPhase::Ended;
        default:
            return ScrollPhase::None;
    }
}

ScrollPhase scrollPhaseFromEvent(NSEvent* event)
{
    // Momentum is reported on its own phase mask; it is checked first because an
    // event coasting after lift-off carries momentumPhase set and phase empty.
    switch (event.momentumPhase)
    {
        case NSEventPhaseBegan:
        case NSEventPhaseChanged:
            return ScrollPhase::Momentum;
        case NSEventPhaseEnded:
        case NSEventPhaseCancelled:
            return ScrollPhase::MomentumEnded;
        default:
            break;
    }

    // A notched wheel reports no phase at all.
    return gesturePhaseFromEvent(event);
}

void scrollWheel(id self, SEL, NSEvent* event)
{
    auto* root = getRootView(self);
    auto windowPos = [event locationInWindow];
    auto localPos = [root convertPoint:windowPos fromView:nil];

    auto e = MouseEvent();

    e.pos = {(float) localPos.x, (float) localPos.y};
    e.type = MouseEventType::Wheel;
    e.button = MouseButton::Other;
    e.modifiers = modifierKeysFromEvent(event);
    e.timestamp = event.timestamp;
    e.preciseScrolling = event.hasPreciseScrollingDeltas == YES;
    e.scrollPhase = scrollPhaseFromEvent(event);

    // scrollingDelta*, not delta*: the scrolling pair carries the precise
    // per-point figure on a trackpad, where deltaY has already been quantised
    // back into whole lines and the smoothness is gone.
    e.delta = {(float) event.scrollingDeltaX, (float) event.scrollingDeltaY};
    e.rawDelta = e.delta;

    e.downPos = e.pos;

    if (auto* view = getView(root))
        view->dispatchMouseEvent(e);
}

void magnifyWithEvent(id self, SEL, NSEvent* event)
{
    auto* root = getRootView(self);
    auto localPos = [root convertPoint:[event locationInWindow] fromView:nil];

    auto e = MouseEvent();

    e.pos = {(float) localPos.x, (float) localPos.y};
    e.downPos = e.pos;
    e.type = MouseEventType::Magnify;
    e.button = MouseButton::Other;
    e.modifiers = modifierKeysFromEvent(event);
    e.timestamp = event.timestamp;
    e.scrollPhase = gesturePhaseFromEvent(event);
    e.magnifyScale = magnifyScaleFromMagnification(event.magnification);

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
    {
        view->resizeStarted();
        view->resized();
        view->resizeFinished();
    }
}

// A view ignoring the pointer answers no hit test, so AppKit carries on to the
// view beneath: that one takes the click, becomes first responder and sets the
// cursor, exactly as if this one were not there.
NSView* hitTest(id self, SEL, NSPoint point)
{
    if (ignoresMouseEvents(self))
        return nil;

    return ObjC::sendSuper<NSView*>(self, [NSView class], @selector(hitTest:), point);
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

// A click into a window whose app is not active is spent on the activation
// itself unless the view claims it, and the window that most needs the click is
// the one that has just come up behind somebody else's. Claiming it puts the
// first press on the control the user aimed at.
BOOL acceptsFirstMouse(id, SEL, NSEvent*)
{
    return YES;
}

void viewDidChangeBackingProperties(id self, SEL)
{
    ObjC::sendSuper<void>(
        self, [NSView class], @selector(viewDidChangeBackingProperties));

    auto* view = (NSView*) self;
    view.layer.contentsScale = view.window.backingScaleFactor;

    // The base layer follows the new scale by itself, but anything the C++ side
    // sized in device pixels (a CAMetalLayer's drawableSize, a glyph atlas) does
    // not, so tell the view.
    if (auto* eacpView = getView(self))
    {
        eacpView->resizeStarted();
        eacpView->backingScaleChanged();
        eacpView->resizeFinished();
    }
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
    if (ignoresMouseEvents(self))
        return;

    dispatchMouseEvent(self, event, MouseEventType::Moved);
}

void mouseEntered(id self, SEL, NSEvent* event)
{
    if (ignoresMouseEvents(self))
        return;

    dispatchMouseEvent(self, event, MouseEventType::Entered);
}

void mouseExited(id self, SEL, NSEvent* event)
{
    if (ignoresMouseEvents(self))
        return;

    dispatchMouseEvent(self, event, MouseEventType::Exited);
}

void keyDown(id self, SEL, NSEvent* event)
{
    if (auto* view = getView(self))
        view->dispatchKeyEvent(keyEventFrom(event, KeyEventType::Down));
}

void keyUp(id self, SEL, NSEvent* event)
{
    if (auto* view = getView(self))
        view->dispatchKeyEvent(keyEventFrom(event, KeyEventType::Up));
}

NSCursor* createImageCursor(const MouseCursor& cursor)
{
    auto image = cursor.getImage();
    auto scale = (CGFloat) cursor.getScale();
    auto hotspot = cursor.getHotspot();

    auto* rep = [[NSBitmapImageRep alloc]
        initWithBitmapDataPlanes:nullptr
                      pixelsWide:image.width()
                      pixelsHigh:image.height()
                   bitsPerSample:8
                 samplesPerPixel:4
                        hasAlpha:YES
                        isPlanar:NO
                  colorSpaceName:NSDeviceRGBColorSpace
                    bitmapFormat:NSBitmapFormatAlphaNonpremultiplied
                     bytesPerRow:image.width() * 4
                    bitsPerPixel:32];

    std::memcpy(rep.bitmapData, image.pixels().data(), image.pixels().size());

    auto size = NSMakeSize(image.width() / scale, image.height() / scale);
    rep.size = size;

    auto* nsImage = [[NSImage alloc] initWithSize:size];
    [nsImage addRepresentation:rep];

    auto* result = [[NSCursor alloc]
        initWithImage:nsImage
              hotSpot:NSMakePoint(hotspot.x / scale, hotspot.y / scale)];

    [nsImage release];
    [rep release];

    return result;
}

NSCursor* imageCursorFor(const MouseCursor& cursor)
{
    auto* cache = cursor.getNativeCache();

    if (*cache == nullptr)
        *cache = std::shared_ptr<void>((void*) createImageCursor(cursor),
                                       [](void* native)
                                       { [(NSCursor*) native release]; });

    return (NSCursor*) cache->get();
}

NSCursor* toNSCursor(const MouseCursor& cursor)
{
    if (cursor.isImage())
        return imageCursorFor(cursor);

    switch (cursor.getShape())
    {
        case MouseCursor::IBeam:
            return [NSCursor IBeamCursor];
        case MouseCursor::PointingHand:
            return [NSCursor pointingHandCursor];
        case MouseCursor::ResizeLeftRight:
            return [NSCursor resizeLeftRightCursor];
        case MouseCursor::ResizeUpDown:
            return [NSCursor resizeUpDownCursor];
        case MouseCursor::Crosshair:
            return [NSCursor crosshairCursor];
        case MouseCursor::Default:
            break;
    }

    return [NSCursor arrowCursor];
}

// AppKit asks this whenever the pointer enters the tracking area, and after
// anything else has had a go at setting the cursor. Without it, a shape set
// from a mouseMoved handler survives only until the pointer crosses a boundary
// and AppKit resets it — which reads as the cursor flickering back at random
// rather than as a missing method.
void cursorUpdate(id self, SEL, id)
{
    if (ignoresMouseEvents(self))
        return;

    if (auto* view = getView(self))
        [toNSCursor(view->getMouseCursor()) set];
}

Vector<std::string> draggedFilePaths(id<NSDraggingInfo> info)
{
    auto paths = Vector<std::string> {};

    NSArray* urls = [[info draggingPasteboard]
        readObjectsForClasses:@[ [NSURL class] ]
                      options:@{NSPasteboardURLReadingFileURLsOnlyKey: @YES}];

    for (NSURL* url in urls)
        if (url.isFileURL && url.path != nil)
            paths.add(url.path.UTF8String);

    return paths;
}

Point dragPosition(id self, id<NSDraggingInfo> info)
{
    auto local = [(NSView*) self convertPoint:[info draggingLocation] fromView:nil];
    return {(float) local.x, (float) local.y};
}

NSDragOperation dragOperationFor(bool accepted)
{
    return accepted ? NSDragOperationCopy : NSDragOperationNone;
}

NSDragOperation draggingEntered(id self, SEL, id<NSDraggingInfo> info)
{
    auto* view = getView(self);

    if (view == nullptr)
        return NSDragOperationNone;

    return dragOperationFor(
        view->fileDragEnter(draggedFilePaths(info), dragPosition(self, info)));
}

NSDragOperation draggingUpdated(id self, SEL, id<NSDraggingInfo> info)
{
    auto* view = getView(self);

    if (view == nullptr)
        return NSDragOperationNone;

    return dragOperationFor(
        view->fileDragMove(draggedFilePaths(info), dragPosition(self, info)));
}

void draggingExited(id self, SEL, id<NSDraggingInfo>)
{
    if (auto* view = getView(self))
        view->fileDragExit();
}

BOOL performDragOperation(id self, SEL, id<NSDraggingInfo> info)
{
    auto* view = getView(self);

    if (view == nullptr)
        return NO;

    view->filesDropped(draggedFilePaths(info), dragPosition(self, info));
    return YES;
}

BOOL wantsPeriodicDraggingUpdates(id, SEL)
{
    return NO;
}

struct FileDragSession
{
    FileDragOptions options;
};

bool fileDragInProgress = false;

FileDragSession* getFileDragSession(id self)
{
    return static_cast<FileDragSession*>(ObjC::getIvar<void*>(self, "fileDragSession"));
}

void setFileDragSession(id self, FileDragSession* session)
{
    ObjC::getIvar<void*>(self, "fileDragSession") = session;
}

NSDragOperation fileDragSourceOperationMask(id self,
                                            SEL,
                                            NSDraggingSession*,
                                            NSDraggingContext context)
{
    auto* session = getFileDragSession(self);
    auto canMove = session != nullptr && session->options.canMove
                   && context == NSDraggingContextOutsideApplication;

    return canMove ? NSDragOperationCopy | NSDragOperationMove : NSDragOperationCopy;
}

void sendMouseUpAfterFileDrag(id self, NSPoint screenPoint)
{
    auto* window = ((NSView*) self).window;

    if (window == nil)
        return;

    auto* event = [NSEvent mouseEventWithType:NSEventTypeLeftMouseUp
                                     location:[window convertPointFromScreen:screenPoint]
                                modifierFlags:[NSEvent modifierFlags]
                                    timestamp:[NSProcessInfo processInfo].systemUptime
                                 windowNumber:window.windowNumber
                                      context:nil
                                  eventNumber:0
                                   clickCount:1
                                     pressure:0.f];

    if (event != nil)
        dispatchMouseEvent(self, event, MouseEventType::Up);
}

void fileDragSessionEnded(id self,
                          SEL,
                          NSDraggingSession*,
                          NSPoint screenPoint,
                          NSDragOperation operation)
{
    auto session = std::unique_ptr<FileDragSession>(getFileDragSession(self));

    if (session == nullptr)
        return;

    setFileDragSession(self, nullptr);
    fileDragInProgress = false;

    sendMouseUpAfterFileDrag(self, screenPoint);

    if (getView(self) != nullptr && session->options.onFinished)
        session->options.onFinished(fileDragResultFor(operation));

    [(NSView*) self release];
}

NSEvent* mouseEventForFileDrag(NSView* view)
{
    auto* event = mouseEventInDispatch != nil ? mouseEventInDispatch : NSApp.currentEvent;

    if (event == nil || view.window == nil || event.window != view.window)
        return nil;

    switch (event.type)
    {
        case NSEventTypeLeftMouseDown:
        case NSEventTypeLeftMouseDragged:
        case NSEventTypeRightMouseDown:
        case NSEventTypeRightMouseDragged:
        case NSEventTypeOtherMouseDown:
        case NSEventTypeOtherMouseDragged:
            return event;
        default:
            return nil;
    }
}

void updateTrackingAreas(id self, SEL)
{
    ObjC::sendSuper<void>(self, [NSView class], @selector(updateTrackingAreas));

    auto* view = (NSView*) self;

    for (NSTrackingArea* area in view.trackingAreas)
        [view removeTrackingArea:area];

    if (ignoresMouseEvents(self))
        return;

    NSTrackingAreaOptions options =
        NSTrackingMouseEnteredAndExited | NSTrackingMouseMoved
        | NSTrackingActiveInKeyWindow | NSTrackingInVisibleRect
        | NSTrackingCursorUpdate;

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
        builder->addIvar<NSPoint>("mouseDownScreenPosition");
        builder->addIvar<void*>("fileDragSession");

        builder->addProtocol(@protocol(CALayerDelegate));
        builder->addProtocol(@protocol(NSDraggingDestination));
        builder->addProtocol(@protocol(NSDraggingSource));

        builder->addMethod(@selector(drawRect:), drawRect);
        builder->addMethod(@selector(drawLayer:inContext:), drawLayerInContext);
        builder->addMethod(@selector(layout), layout);
        builder->addMethod(@selector(hitTest:), hitTest);
        builder->addMethod(@selector(isFlipped), isFlipped);
        builder->addMethod(@selector(isOpaque), isOpaque);
        builder->addMethod(@selector(acceptsFirstResponder),
                           acceptsFirstResponder);
        builder->addMethod(@selector(acceptsFirstMouse:), acceptsFirstMouse);
        builder->addMethod(@selector(viewDidChangeBackingProperties),
                           viewDidChangeBackingProperties);
        builder->addMethod(@selector(setFrame:), setFrame);

        builder->addMethod(@selector(mouseDown:), mouseDown);
        builder->addMethod(@selector(mouseUp:), mouseUp);
        builder->addMethod(@selector(mouseDragged:), mouseDragged);
        builder->addMethod(@selector(mouseMoved:), mouseMoved);
        builder->addMethod(@selector(mouseEntered:), mouseEntered);
        builder->addMethod(@selector(mouseExited:), mouseExited);
        builder->addMethod(@selector(scrollWheel:), scrollWheel);
        builder->addMethod(@selector(magnifyWithEvent:), magnifyWithEvent);
        builder->addMethod(@selector(rightMouseDown:), mouseDown);
        builder->addMethod(@selector(rightMouseUp:), mouseUp);
        builder->addMethod(@selector(rightMouseDragged:), mouseDragged);
        builder->addMethod(@selector(otherMouseDown:), mouseDown);
        builder->addMethod(@selector(otherMouseUp:), mouseUp);
        builder->addMethod(@selector(otherMouseDragged:), mouseDragged);

        builder->addMethod(@selector(keyDown:), keyDown);
        builder->addMethod(@selector(keyUp:), keyUp);
        builder->addMethod(@selector(cursorUpdate:), cursorUpdate);
        builder->addMethod(@selector(updateTrackingAreas),
                           updateTrackingAreas);

        builder->addMethod(@selector(draggingEntered:), draggingEntered);
        builder->addMethod(@selector(draggingUpdated:), draggingUpdated);
        builder->addMethod(@selector(draggingExited:), draggingExited);
        builder->addMethod(@selector(performDragOperation:), performDragOperation);
        builder->addMethod(@selector(wantsPeriodicDraggingUpdates),
                           wantsPeriodicDraggingUpdates);

        builder->addMethod(
            @selector(draggingSession:sourceOperationMaskForDraggingContext:),
            fileDragSourceOperationMask);
        builder->addMethod(@selector(draggingSession:endedAtPoint:operation:),
                           fileDragSessionEnded);

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

bool isFrameworkNativeView(NSView* view)
{
    return view != nil && [view isKindOfClass:getNativeViewClass()];
}

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

    // AppKit propagates this to the whole subtree on its own, and that includes
    // any nested platform surface: a WKWebView under a hidden ancestor is sent
    // viewDidHide, which is what drops its page out of the visible state.
    void setVisible(bool visible) { nativeView.get().hidden = !visible; }

    Rect getBounds() const { return toRect([nativeView.get() frame]); }
    void setBounds(const Rect& bounds)
    {
        auto frame = toCGRect(bounds);
        [nativeView.get() setFrame:frame];
    }

    float backingScale() const
    {
        auto* view = nativeView.get();
        auto scale = view.window != nil ? view.window.backingScaleFactor
                                        : [NSScreen mainScreen].backingScaleFactor;

        // No window and no screen (headless) reports 0; fall back to 1:1 so the
        // default-scale snapshot still produces pixels.
        return scale > 0.0 ? (float) scale : 1.0f;
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

View& View::setAcceptsFileDrops(bool value)
{
    properties.acceptsFileDrops = value;

    auto* native = (NSView*) impl->nativeView.get();

    if (value)
        [native registerForDraggedTypes:@[ NSPasteboardTypeFileURL ]];
    else
        [native unregisterDraggedTypes];

    return *this;
}

bool View::startFileDrag(const Vector<std::string>& paths,
                         const FileDragOptions& options)
{
    auto* native = (NSView*) impl->nativeView.get();

    if (paths.empty() || fileDragInProgress)
        return false;

    auto* event = mouseEventForFileDrag(native);

    if (event == nil)
        return false;

    auto anchor = [native convertPoint:event.locationInWindow fromView:nil];
    auto* items = makeFileDraggingItems(
        native, anchor, paths, options.image, options.imageScale);

    if (items.count == 0)
        return false;

    setFileDragSession(native, new FileDragSession {options});
    fileDragInProgress = true;
    [native retain];

    auto* session = [native beginDraggingSessionWithItems:items
                                                    event:event
                                                   source:(id<NSDraggingSource>) native];

    if (session == nil)
    {
        delete getFileDragSession(native);
        setFileDragSession(native, nullptr);
        fileDragInProgress = false;
        [native release];
        return false;
    }

    session.animatesToStartingPositionsOnCancelOrFail = YES;
    return true;
}

void View::setMouseCursor(const MouseCursor& cursor)
{
    if (currentCursor == cursor)
        return;

    currentCursor = cursor;

    // Applied here as well as from cursorUpdate:, because the call that changes
    // it almost always comes *from* a mouseMoved handler — the pointer is
    // already inside, and inside is the only time the shape is visible. Waiting
    // for the next cursorUpdate: would leave the old shape under a pointer that
    // has already crossed onto the splitter.
    auto* native = (NSView*) impl->nativeView.get();

    if (native != nil && native.window != nil)
        [toNSCursor(cursor) set];
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
