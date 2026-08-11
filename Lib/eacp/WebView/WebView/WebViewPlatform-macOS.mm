#import <AppKit/AppKit.h>
#import <WebKit/WebKit.h>

#include "WebViewPlatform-Apple.h"

#include <eacp/Core/ObjC/ObjC.h>
#include <eacp/Core/ObjC/RuntimeClass.h>
#include <eacp/Core/ObjC/Strings.h>

namespace eacp::Graphics::detail
{
namespace
{
// Files travel as public.file-url pasteboard items, not file promises, which
// targets that read only file-urls silently drop.
NSDragOperation dragSourceOperationMask(id, SEL, NSDraggingSession*, NSDraggingContext)
{
    return NSDragOperationCopy;
}

Class getDragSourceClass()
{
    static auto instance = []
    {
        auto builder = new ObjC::RuntimeClass<NSObject>("EacpDragSource");

        builder->addProtocol(@protocol(NSDraggingSource));
        builder->addMethod(
            @selector(draggingSession:sourceOperationMaskForDraggingContext:),
            dragSourceOperationMask);

        builder->registerClass();
        return builder;
    }();

    return instance->get();
}

// Stateless, so one instance safely outlives every drag session.
id<NSDraggingSource> sharedDragSource()
{
    static id<NSDraggingSource> source =
        (id<NSDraggingSource>) [[getDragSourceClass() alloc] init];
    return source;
}
} // namespace

bool beginFileDrag(WKWebView* webView,
                   NSEvent* event,
                   const Vector<std::string>& paths)
{
    if (webView == nil || event == nil || paths.empty())
        return false;

    auto* source = sharedDragSource();
    constexpr CGFloat iconSize = 64.0;
    constexpr CGFloat stackOffset = 8.0;

    auto* items = [NSMutableArray arrayWithCapacity:paths.size()];
    auto* workspace = [NSWorkspace sharedWorkspace];
    auto anchor = [webView convertPoint:event.locationInWindow fromView:nil];

    NSUInteger index = 0;
    for (const auto& path: paths)
    {
        auto* nsPath = Strings::toNSString(path);
        auto* fileURL = [NSURL fileURLWithPath:nsPath];

        if (fileURL == nil)
            continue;

        auto* item = [[NSDraggingItem alloc] initWithPasteboardWriter:fileURL];

        auto* icon = [workspace iconForFile:nsPath];
        icon.size = NSMakeSize(iconSize, iconSize);

        // Fanned out, so a multi-file drag reads as multiple items.
        auto offset = (CGFloat) index * stackOffset;
        [item setDraggingFrame:NSMakeRect(anchor.x - iconSize / 2 + offset,
                                          anchor.y - iconSize / 2 - offset,
                                          iconSize,
                                          iconSize)
                      contents:icon];

        [items addObject:item];
        [item release];
        ++index;
    }

    if (items.count == 0)
        return false;

    [webView beginDraggingSessionWithItems:items event:event source:source];
    return true;
}

namespace
{
// Stashed events this old never got a page verdict; dropping them beats
// mispairing them with a later one.
constexpr NSTimeInterval keyEventExpirySeconds = 2.0;
constexpr int maxPendingKeyEvents = 64;

void dropExpiredKeyEvents(Vector<ObjC::Ptr<NSEvent>>& queue)
{
    auto now = [NSProcessInfo processInfo].systemUptime;
    queue.eraseIf([now](auto& event)
                  { return now - event.get().timestamp > keyEventExpirySeconds; });
}

// State of the drag-out WKWebView subclass, behind a raw pointer because
// runtime classes construct no C++ ivars. The drag must start from a genuine
// NSEventTypeLeftMouseDragged, or it stays confined to the app.
struct DragWebViewState
{
    Vector<std::string> armedPaths;
    Callback fileDragStartedCallback;
    UnhandledNSKeyCallback unhandledKeyCallback;
    Vector<ObjC::Ptr<NSEvent>> pendingKeyDowns;
    Vector<ObjC::Ptr<NSEvent>> pendingKeyUps;
    NSPoint mouseDownLocation {};
    bool dragArmed = false;
    bool dragStarted = false;
    bool windowDragArmed = false;
    bool acceptFirstMouse = false;
};

DragWebViewState* getDragWebViewState(id self)
{
    return (DragWebViewState*) ObjC::getIvar<void*>(self, "state");
}

BOOL dragWebViewAcceptsFirstMouse(id self, SEL, NSEvent* event)
{
    if (getDragWebViewState(self)->acceptFirstMouse)
        return YES;

    return ObjC::sendSuper<BOOL>(
        self, [WKWebView class], @selector(acceptsFirstMouse:), event);
}

void dragWebViewMouseDown(id self, SEL, NSEvent* event)
{
    auto* view = (WKWebView*) self;
    auto* state = getDragWebViewState(self);

    // WKWebView joins AppKit's delayed-window-ordering protocol, which
    // suppresses the click-to-focus a plain NSView gets for free.
    if (view.window != nil && !view.window.keyWindow
        && view.window.canBecomeKeyWindow)
        [view.window makeKeyWindow];

    state->mouseDownLocation = event.locationInWindow;
    state->dragArmed = false;
    state->dragStarted = false;
    state->windowDragArmed = false;
    state->armedPaths.clear();
    ObjC::sendSuper<void>(self, [WKWebView class], @selector(mouseDown:), event);
}

void dragWebViewMouseDragged(id self, SEL, NSEvent* event)
{
    auto* view = (WKWebView*) self;
    auto* state = getDragWebViewState(self);

    if (state->dragArmed && ! state->dragStarted)
    {
        auto dx = event.locationInWindow.x - state->mouseDownLocation.x;
        auto dy = event.locationInWindow.y - state->mouseDownLocation.y;
        constexpr CGFloat threshold = 4.0;

        if (dx * dx + dy * dy >= threshold * threshold)
        {
            state->dragStarted = true;
            state->dragArmed = false;
            auto didStart = beginFileDrag(view, event, state->armedPaths);
            state->armedPaths.clear();
            if (didStart && state->fileDragStartedCallback)
            {
                auto callback = state->fileDragStartedCallback;
                dispatch_async(dispatch_get_main_queue(),
                               ^{ callback(); });
            }
            return;
        }
    }

    if (state->windowDragArmed && ! state->dragStarted)
    {
        state->dragStarted = true;
        state->windowDragArmed = false;
        [view.window performWindowDragWithEvent:event];
        return;
    }

    ObjC::sendSuper<void>(
        self, [WKWebView class], @selector(mouseDragged:), event);
}

void dragWebViewMouseUp(id self, SEL, NSEvent* event)
{
    auto* state = getDragWebViewState(self);

    state->dragArmed = false;
    state->dragStarted = false;
    state->windowDragArmed = false;
    state->armedPaths.clear();
    ObjC::sendSuper<void>(self, [WKWebView class], @selector(mouseUp:), event);
}

// WebKit re-sends an unhandled key-down through the responder chain, so the
// same NSEvent hits keyDown: twice while the page reports only one verdict.
bool isAlreadyStashed(NSEvent* event, const Vector<ObjC::Ptr<NSEvent>>& queue)
{
    for (auto& stashed: queue)
    {
        if (stashed.get() == event
            || (stashed.get().timestamp == event.timestamp
                && stashed.get().keyCode == event.keyCode))
            return true;
    }

    return false;
}

void stashKeyEvent(NSEvent* event, Vector<ObjC::Ptr<NSEvent>>& queue)
{
    dropExpiredKeyEvents(queue);

    if (isAlreadyStashed(event, queue))
        return;

    if (queue.size() >= maxPendingKeyEvents)
        queue.removeAt(0);

    auto retained = ObjC::Ptr<NSEvent>();
    retained.reset(event);
    queue.add(retained);
}

// Cmd combos travel the key-equivalent path, not keyDown:, and the page shim
// skips them symmetrically — stashing one here would desync the verdict queue.
bool shouldStashKeyEvent(id self, NSEvent* event)
{
    return getDragWebViewState(self)->unhandledKeyCallback != nullptr
           && (event.modifierFlags & NSEventModifierFlagCommand) == 0;
}

void dragWebViewKeyDown(id self, SEL, NSEvent* event)
{
    if (shouldStashKeyEvent(self, event))
        stashKeyEvent(event, getDragWebViewState(self)->pendingKeyDowns);

    ObjC::sendSuper<void>(self, [WKWebView class], @selector(keyDown:), event);
}

void dragWebViewKeyUp(id self, SEL, NSEvent* event)
{
    if (shouldStashKeyEvent(self, event))
        stashKeyEvent(event, getDragWebViewState(self)->pendingKeyUps);

    ObjC::sendSuper<void>(self, [WKWebView class], @selector(keyUp:), event);
}

void deallocDragWebView(id self, SEL)
{
    delete getDragWebViewState(self);
    ObjC::sendSuper<void>(self, [WKWebView class], @selector(dealloc));
}

Class getDragWebViewClass()
{
    static auto instance = []
    {
        auto builder = new ObjC::RuntimeClass<WKWebView>("EacpDragWebView");

        builder->addIvar<void*>("state");

        builder->addMethod(@selector(acceptsFirstMouse:),
                           dragWebViewAcceptsFirstMouse);
        builder->addMethod(@selector(mouseDown:), dragWebViewMouseDown);
        builder->addMethod(@selector(mouseDragged:), dragWebViewMouseDragged);
        builder->addMethod(@selector(mouseUp:), dragWebViewMouseUp);
        builder->addMethod(@selector(keyDown:), dragWebViewKeyDown);
        builder->addMethod(@selector(keyUp:), dragWebViewKeyUp);
        builder->addMethod(@selector(dealloc), deallocDragWebView);

        builder->registerClass();
        return builder;
    }();

    return instance->get();
}
} // namespace

WKWebView* createWebView(WKWebViewConfiguration* config,
                         const WebKitOptions& options)
{
    auto rect = CGRectMake(0, 0, 100, 100);
    auto* webView = (WKWebView*) [[getDragWebViewClass() alloc]
        initWithFrame:rect
        configuration:config];
    ObjC::getIvar<void*>(webView, "state") = new DragWebViewState();
    getDragWebViewState(webView)->acceptFirstMouse = options.acceptFirstMouse;
    return webView;
}

void armFileDrag(WKWebView* webView, const Vector<std::string>& paths)
{
    if (![webView isKindOfClass:getDragWebViewClass()])
        return;

    auto* state = getDragWebViewState(webView);
    state->armedPaths = paths;
    state->dragArmed = ! paths.empty();
}

void setFileDragStartedCallback(WKWebView* webView, Callback callback)
{
    if (![webView isKindOfClass:getDragWebViewClass()])
        return;

    getDragWebViewState(webView)->fileDragStartedCallback = std::move(callback);
}

void armWindowDrag(WKWebView* webView)
{
    if (![webView isKindOfClass:getDragWebViewClass()])
        return;

    getDragWebViewState(webView)->windowDragArmed = true;
}

void setUnhandledKeyCallback(WKWebView* webView, UnhandledNSKeyCallback callback)
{
    if (![webView isKindOfClass:getDragWebViewClass()])
        return;

    getDragWebViewState(webView)->unhandledKeyCallback = std::move(callback);
}

void reportKeyVerdict(WKWebView* webView, bool isDown, bool consumed)
{
    if (![webView isKindOfClass:getDragWebViewClass()])
        return;

    auto* state = getDragWebViewState(webView);
    auto& queue = isDown ? state->pendingKeyDowns : state->pendingKeyUps;
    dropExpiredKeyEvents(queue);

    if (queue.empty())
        return; // verdict from a page state we no longer track (navigation)

    auto event = queue[0];
    queue.removeAt(0);

    if (!consumed && state->unhandledKeyCallback != nullptr)
        state->unhandledKeyCallback(event.get(), isDown);
}

void performWindowControl(WKWebView* webView, const std::string& action)
{
    NSWindow* window = webView.window;
    if (window == nil)
        return;

    if (action == "minimize")
    {
        [window miniaturize:nil];
        return;
    }

    if (action == "maximize")
    {
        // zoom: is itself a toggle, so report the resulting state back.
        [window zoom:nil];
        auto* script = window.zoomed ? @"window.__eacpSetMaximized(true)"
                                     : @"window.__eacpSetMaximized(false)";
        [webView evaluateJavaScript:script completionHandler:nil];
        return;
    }

    if (action == "close")
    {
        // performClose: beeps and refuses on windows without a close button —
        // exactly the frameless ones needing web-rendered controls.
        if ((window.styleMask & NSWindowStyleMaskClosable) != 0)
            [window performClose:nil];
        else
            [window close];
    }
}

void attachWKWebViewToParent(WKWebView* webView, void* parentHandle)
{
    auto* parentView = (__bridge NSView*) parentHandle;
    [parentView addSubview:webView];
}

void applyNativeZoom(WKWebView* webView, double clamped, double&)
{
    webView.pageZoom = clamped;
}

double readNativeZoom(WKWebView* webView, double)
{
    return webView.pageZoom;
}

WebView* findFocusedWebView()
{
    auto* keyWindow = [NSApp keyWindow];

    if (keyWindow == nil)
        return nullptr;

    for (auto* view: registeredWebViews())
    {
        auto* wkWebView = wkWebViewOf(view);

        if (wkWebView != nil && wkWebView.window == keyWindow)
            return view;
    }

    return nullptr;
}
} // namespace eacp::Graphics::detail
