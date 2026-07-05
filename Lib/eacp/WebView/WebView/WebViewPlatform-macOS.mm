#import <AppKit/AppKit.h>
#import <WebKit/WebKit.h>

#include "WebViewPlatform-Apple.h"

#include <eacp/Core/ObjC/Strings.h>

// NSDraggingSource for native file drag-out. We drag real on-disk files as
// public.file-url pasteboard items (NSURL is an NSPasteboardWriting), which is
// the representation Finder, DAWs, and virtually every drop target understand
// -- unlike a file promise, which apps that read only file-urls silently drop.
@interface EacpDragSource : NSObject <NSDraggingSource>
@end

@implementation EacpDragSource

- (NSDragOperation)draggingSession:(NSDraggingSession*)session
    sourceOperationMaskForDraggingContext:(NSDraggingContext)context
{
    return NSDragOperationCopy;
}

@end

// WKWebView subclass that owns native file drag-out. The page arms a drag on
// mousedown (via window.eacp.armFileDrag); by the time the pointer crosses the
// drag threshold the armed paths have arrived over the message channel, so we
// start the session from the genuine NSEventTypeLeftMouseDragged event. That
// real, OS-delivered event is what lets the drag escape the app -- a session
// started from the async script-message callback is confined to the app
// (NSDraggingContextWithinApplication) and never reaches Finder.
@interface EacpDragWebView : WKWebView
@property(nonatomic) BOOL eacpAcceptFirstMouse;
- (void)armFileDragWithPaths:(const eacp::Vector<std::string>&)paths;
- (void)setFileDragStartedCallback:(eacp::Callback)callback;
- (void)armWindowDrag;
@end

namespace eacp::Graphics::detail
{
namespace
{
// One shared drag source for the whole app -- it is stateless apart from the
// work queue, so a singleton is safe and outlives every drag session.
EacpDragSource* sharedDragSource()
{
    static auto* source = [[EacpDragSource alloc] init];
    return source;
}
} // namespace

// A multi-file drag fans the icon stack out a few px per item so it reads
// as multiple items rather than one.
bool beginFileDrag(WKWebView* webView,
                   NSEvent* event,
                   const Vector<std::string>& paths)
{
    if (webView == nil || event == nil || paths.empty())
        return false;

    auto* source = sharedDragSource();
    constexpr auto iconSize = 64.0;
    constexpr auto stackOffset = 8.0;

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

WKWebView* createWebView(WKWebViewConfiguration* config,
                         const WebKitOptions& options)
{
    auto rect = CGRectMake(0, 0, 100, 100);
    auto* webView = [[EacpDragWebView alloc] initWithFrame:rect
                                             configuration:config];
    webView.eacpAcceptFirstMouse = options.acceptFirstMouse;
    return webView;
}

void armFileDrag(WKWebView* webView, const Vector<std::string>& paths)
{
    if (![webView isKindOfClass:[EacpDragWebView class]])
        return;

    [(EacpDragWebView*) webView armFileDragWithPaths:paths];
}

void setFileDragStartedCallback(WKWebView* webView, Callback callback)
{
    if (![webView isKindOfClass:[EacpDragWebView class]])
        return;

    [(EacpDragWebView*) webView setFileDragStartedCallback:std::move(callback)];
}

void armWindowDrag(WKWebView* webView)
{
    if (![webView isKindOfClass:[EacpDragWebView class]])
        return;

    [(EacpDragWebView*) webView armWindowDrag];
}

// "maximize" uses zoom:, which is itself a toggle — it restores the saved
// frame when the window is already zoomed — and the resulting state is
// reported back so the page's data-eacp-maximized attribute tracks reality.
//
// "close" prefers performClose:, which respects windowShouldClose:, but it
// beeps and refuses on windows without a close button — exactly the
// frameless windows that need web-rendered controls — so those close
// directly. windowWillClose still fires either way, so the window's quit
// policy runs.
void performWindowControl(WKWebView* webView, const std::string& action)
{
    auto window = webView.window;
    if (window == nil)
        return;

    if (action == "minimize")
    {
        [window miniaturize:nil];
        return;
    }

    if (action == "maximize")
    {
        [window zoom:nil];
        auto* script = window.zoomed ? @"window.__eacpSetMaximized(true)"
                                     : @"window.__eacpSetMaximized(false)";
        [webView evaluateJavaScript:script completionHandler:nil];
        return;
    }

    if (action == "close")
    {
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

@implementation EacpDragWebView
{
    eacp::Vector<std::string> armedPaths;
    eacp::Callback fileDragStartedCallback;
    NSPoint mouseDownLocation;
    BOOL dragArmed;
    BOOL dragStarted;
    BOOL windowDragArmed;
}

- (void)armFileDragWithPaths:(const eacp::Vector<std::string>&)paths
{
    // Always lands after this gesture's mouseDown: (the page's JS mousedown is
    // dispatched only once our mouseDown: forwards to super), so there is
    // nothing stale to guard against here.
    armedPaths = paths;
    dragArmed = ! paths.empty();
}

- (void)setFileDragStartedCallback:(eacp::Callback)callback
{
    fileDragStartedCallback = std::move(callback);
}

- (void)armWindowDrag
{
    windowDragArmed = YES;
}

// With eacpAcceptFirstMouse set, the click that activates an unfocused
// window also reaches the page — so a drag region starts moving the window
// on the FIRST click-drag instead of needing one click to focus first.
- (BOOL)acceptsFirstMouse:(NSEvent*)event
{
    if (self.eacpAcceptFirstMouse)
        return YES;
    return [super acceptsFirstMouse:event];
}

- (void)mouseDown:(NSEvent*)event
{
    mouseDownLocation = event.locationInWindow;
    dragArmed = NO;
    dragStarted = NO;
    windowDragArmed = NO;
    armedPaths.clear();
    [super mouseDown:event];
}

- (void)mouseDragged:(NSEvent*)event
{
    if (dragArmed && ! dragStarted)
    {
        auto dx = event.locationInWindow.x - mouseDownLocation.x;
        auto dy = event.locationInWindow.y - mouseDownLocation.y;
        constexpr auto threshold = 4.0;

        if (dx * dx + dy * dy >= threshold * threshold)
        {
            dragStarted = YES;
            dragArmed = NO;
            auto didStart =
                eacp::Graphics::detail::beginFileDrag(self, event, armedPaths);
            armedPaths.clear();
            if (didStart && fileDragStartedCallback)
            {
                auto callback = fileDragStartedCallback;
                dispatch_async(dispatch_get_main_queue(),
                               ^{ callback(); });
            }
            return;
        }
    }

    if (windowDragArmed && ! dragStarted)
    {
        dragStarted = YES;
        windowDragArmed = NO;
        [self.window performWindowDragWithEvent:event];
        return;
    }

    [super mouseDragged:event];
}

- (void)mouseUp:(NSEvent*)event
{
    dragArmed = NO;
    dragStarted = NO;
    windowDragArmed = NO;
    armedPaths.clear();
    [super mouseUp:event];
}

@end
