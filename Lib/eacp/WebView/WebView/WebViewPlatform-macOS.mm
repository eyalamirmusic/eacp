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
// NSDraggingSource for native file drag-out. We drag real on-disk files as
// public.file-url pasteboard items (NSURL is an NSPasteboardWriting), which is
// the representation Finder, DAWs, and virtually every drop target understand
// -- unlike a file promise, which apps that read only file-urls silently drop.
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

// One shared drag source for the whole app -- it is stateless apart from the
// work queue, so a singleton is safe and outlives every drag session.
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

        // Fan the stack out a few px per item so a multi-file drag reads as
        // multiple items rather than one.
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
// Stashed key events older than this have lost their page verdict (a
// navigation raced the report, or WebKit never dispatched a DOM event for
// them) and are dropped rather than mispaired with a later verdict.
constexpr NSTimeInterval keyEventExpirySeconds = 2.0;
constexpr int maxPendingKeyEvents = 64;

void dropExpiredKeyEvents(Vector<ObjC::Ptr<NSEvent>>& queue)
{
    auto now = [NSProcessInfo processInfo].systemUptime;
    queue.eraseIf([now](auto& event)
                  { return now - event.get().timestamp > keyEventExpirySeconds; });
}

// WKWebView subclass that owns native file drag-out. The page arms a drag on
// mousedown (via window.eacp.armFileDrag); by the time the pointer crosses the
// drag threshold the armed paths have arrived over the message channel, so we
// start the session from the genuine NSEventTypeLeftMouseDragged event. That
// real, OS-delivered event is what lets the drag escape the app -- a session
// started from the async script-message callback is confined to the app
// (NSDraggingContextWithinApplication) and never reaches Finder.
//
// Runtime classes get no automatic C++ ivar construction, so the view's C++
// state lives behind one raw "state" pointer, created with the view and
// deleted in its dealloc.
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

// With acceptFirstMouse set, the click that activates an unfocused
// window also reaches the page — so a drag region starts moving the window
// on the FIRST click-drag instead of needing one click to focus first.
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

    // WKWebView takes part in AppKit's delayed-window-ordering protocol (for
    // dragging content out of background windows), which suppresses the
    // click-to-focus a plain NSView gets for free — without this, a click on
    // an unfocused window only landed focus after a drag or a second click.
    // Claim key status explicitly before the page sees the click.
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

// The same NSEvent can hit the keyDown: override twice: when the page leaves
// a key unhandled, WebKit re-sends the original event through the responder
// chain (_resendKeyDownEvent), and the re-send lands back on this view first.
// The page shim only reports ONE verdict per event, so stashing the re-send
// would leave a stale entry that desyncs every later verdict — key-downs then
// pop the wrong stashed event, while key-ups (never re-sent) stay aligned.
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

    // Always lands after this gesture's mouseDown: (the page's JS mousedown is
    // dispatched only once our mouseDown: forwards to super), so there is
    // nothing stale to guard against here.
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
        // zoom: is itself a toggle — it restores the saved frame when the
        // window is already zoomed. Report the resulting state back so the
        // page's data-eacp-maximized attribute tracks reality.
        [window zoom:nil];
        auto* script = window.zoomed ? @"window.__eacpSetMaximized(true)"
                                     : @"window.__eacpSetMaximized(false)";
        [webView evaluateJavaScript:script completionHandler:nil];
        return;
    }

    if (action == "close")
    {
        // performClose: respects windowShouldClose:, but beeps and refuses
        // on windows without a close button — exactly the frameless windows
        // that need web-rendered controls — so those close directly.
        // windowWillClose still fires either way, so the window's quit
        // policy runs.
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
