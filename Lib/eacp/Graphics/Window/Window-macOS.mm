#include "Window.h"
#include "MouseLock-macOS.h"
#include "Window-macOS.h"
#include "../Graphics/Keyboard.h"
#include "../Helpers/ImageConversion-macOS.h"
#include "../Primitives/GraphicUtils.h"
#include <eacp/Core/ObjC/RuntimeClass.h>
#include <eacp/Core/Threads/EventLoop.h>
#import <Cocoa/Cocoa.h>

// std::min, for holding a window inside its screen's visible frame.
#include <algorithm>

namespace
{
// Reposition the standard window controls to sit `inset` points from the
// window's top-left, preserving the system spacing between them. Mirrors how
// Electron implements trafficLightPosition — there is no NSWindow API for it,
// so we move the buttons directly. Skipped in fullscreen, where macOS owns
// their placement.
void repositionTrafficLights(NSWindow* window, NSPoint inset)
{
    if (window.styleMask & NSWindowStyleMaskFullScreen)
        return;

    NSButton* close = [window standardWindowButton:NSWindowCloseButton];
    NSButton* miniaturize =
        [window standardWindowButton:NSWindowMiniaturizeButton];
    NSButton* zoom = [window standardWindowButton:NSWindowZoomButton];

    if (close == nil || miniaturize == nil || zoom == nil)
        return;

    NSView* container = close.superview;
    CGFloat containerHeight = NSHeight(container.frame);
    CGFloat spacing = NSMinX(miniaturize.frame) - NSMinX(close.frame);

    NSButton* buttons[] = {close, miniaturize, zoom};
    for (int i = 0; i < 3; ++i)
    {
        NSRect frame = buttons[i].frame;
        frame.origin.x = inset.x + i * spacing;
        frame.origin.y = containerHeight - inset.y - NSHeight(frame);
        buttons[i].frame = frame;
    }
}

// Ask the system to bring this app to the foreground.
//
// Activation is COOPERATIVE since macOS 14: activateIgnoringOtherApps: is
// documented as deprecated and demoted to a plain -activate, which the system
// declines while the user is working in another app — exactly the
// launched-from-a-terminal / IDE case. Measured on macOS 26, the demotion is
// not what happens: -activate never lands for a terminal-launched app, still
// inactive twelve seconds later, while activateIgnoringOtherApps: lands in
// about 20 ms every time. So the deprecated call is the request, and
// reopenSelfViaLaunchServices below is the escalation for the day the
// documented behaviour becomes the real one.
void requestActivation()
{
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
    [NSApp activateIgnoringOtherApps:YES];
#pragma clang diagnostic pop
}

// Ask LaunchServices to "open" this app. An open of an already-running app is
// a user-level activation the system honours even while another app is
// receiving input — unlike the cooperative request above. Bundled apps only:
// "opening" a bare dev executable would misfire.
void reopenSelfViaLaunchServices()
{
    auto* bundle = [NSBundle mainBundle];
    if (! [bundle.bundlePath.pathExtension isEqualToString:@"app"])
        return;

    auto* configuration = [NSWorkspaceOpenConfiguration configuration];
    configuration.activates = YES;
    [[NSWorkspace sharedWorkspace] openApplicationAtURL:bundle.bundleURL
                                          configuration:configuration
                                      completionHandler:nil];
}

constexpr auto activationPollSeconds = 0.25;
constexpr auto activationAttempts = 8;

// Bring the app to the foreground.
//
// The request above is what normally lands, so the poller is for the case
// where it is refused, and the LaunchServices re-open goes first among the
// retries because it is the one that has never been refused — a quarter second
// rather than the second and a quarter it used to take to reach it.
//
// Stops the moment the app is active. Staying on to watch for a grant being
// taken back would mean taking the screen off a user who deliberately switched
// away just after launch, which is worse than the launch that failed to come
// forward; measured, the request above is not revoked, and only the refused
// cooperative one ever was. One shared poller — toFront() is called once per
// window at startup, and overlapping retry chains would just spam the request.
void ensureAppBecomesActive()
{
    static auto polling = false;
    if (polling || NSApp.active)
        return;

    requestActivation();
    polling = true;

    __block auto attempt = 0;
    [NSTimer scheduledTimerWithTimeInterval:activationPollSeconds
                                    repeats:YES
                                      block:^(NSTimer* timer)
                                      {
                                          ++attempt;

                                          if (NSApp.active
                                              || attempt > activationAttempts)
                                          {
                                              polling = false;
                                              [timer invalidate];
                                              return;
                                          }

                                          if (attempt % 2 == 1)
                                              reopenSelfViaLaunchServices();
                                          else
                                              requestActivation();
                                      }];
}
} // namespace

namespace eacp::Graphics
{
namespace
{
// Borderless NSWindows refuse key status by default, which would make a
// frameless overlay's text inputs untypeable. Same override Electron ships
// for frame:false windows.
BOOL canBecomeKeyWindow(id, SEL)
{
    return YES;
}

// A popup is the opposite promise: it must never take key or main status, so
// the window it pops over stays active while the menu is up. The
// nonactivating mask alone is not enough — it governs the app, not the window.
BOOL refusesKeyWindow(id, SEL)
{
    return NO;
}

Class getKeyableBorderlessWindowClass()
{
    static auto instance = []
    {
        auto builder =
            new ObjC::RuntimeClass<NSWindow>("EacpKeyableBorderlessWindow");
        builder->addMethod(@selector(canBecomeKeyWindow), canBecomeKeyWindow);
        builder->registerClass();
        return builder;
    }();

    return instance->get();
}

Class getPopupPanelClass()
{
    static auto instance = []
    {
        auto builder = new ObjC::RuntimeClass<NSPanel>("EacpPopupPanel");
        builder->addMethod(@selector(canBecomeKeyWindow), refusesKeyWindow);
        builder->addMethod(@selector(canBecomeMainWindow), refusesKeyWindow);
        builder->registerClass();
        return builder;
    }();

    return instance->get();
}

// The masks AppKit only implements on NSPanel. Set on a plain NSWindow they
// are silently nothing, which is what made NonactivatingPanel, HUDWindow and
// UtilityWindow dead flags until the class was chosen from them.
constexpr NSWindowStyleMask panelOnlyStyleMask =
    NSWindowStyleMaskUtilityWindow | NSWindowStyleMaskDocModalWindow
    | NSWindowStyleMaskNonactivatingPanel | NSWindowStyleMaskHUDWindow;

// AppKit measures screen points from the bottom-left of the primary screen
// with y growing up; eacp measures them from its top-left with y growing down
// (see Display, and WindowOptions::initialPosition). Both directions of the
// conversion are the same flip about the primary screen's top edge.
double primaryScreenTop()
{
    NSScreen* primary = NSScreen.screens.firstObject;
    return primary != nil ? NSMaxY(primary.frame) : 0.0;
}

Point toScreenPoint(NSRect frame)
{
    return screenPointFromAppKit(CGPointMake(frame.origin.x, NSMaxY(frame)));
}

// Runtime classes get no automatic C++ ivar construction, so the delegate's
// C++ state lives behind one raw pointer, created with the delegate and
// deleted in its dealloc.
struct WindowDelegateState
{
    Callback cb = [] {};
    ResizeCallback onResize;
    SizeConstraint sizeConstraint;

    // The content size when the current live resize began; see
    // windowWillResize.
    std::optional<Point> liveResizeStart;
    bool hidesOnClose = false;
    WindowEvents* events = nullptr;
    // Internal key-focus listener (mouse lock suspend/resume), invoked
    // alongside the user-facing events->onActivationChanged.
    std::function<void(bool)> onKeyStateChanged;
    bool keepTrafficLightsPositioned = false;
    NSPoint trafficLightInset {};
};

WindowDelegateState* getDelegateState(id self)
{
    return (WindowDelegateState*) ObjC::getIvar<void*>(self, "state");
}

void windowWillClose(id self, SEL, NSNotification*)
{
    getDelegateState(self)->cb();
}

// hidesOnClose intercepts the close before it happens: the window orders
// out (state intact, willClose never fires) and the app keeps running.
// events->onHidden is the app's only sign that any of it happened.
BOOL windowShouldClose(id self, SEL, NSWindow* sender)
{
    auto* state = getDelegateState(self);

    if (!state->hidesOnClose)
        return YES;

    [sender orderOut:nil];

    // After the orderOut, so a handler asking isVisible() is told the truth.
    if (state->events != nullptr)
        state->events->onHidden();

    return NO;
}

Point currentContentSize(NSWindow* window)
{
    auto content = [window contentRectForFrameRect:[window frame]].size;
    return {(float) content.width, (float) content.height};
}

Point contentSizeForFrameSize(NSWindow* window, NSSize frameSize)
{
    auto frame = NSMakeRect(0, 0, frameSize.width, frameSize.height);
    auto content = [window contentRectForFrameRect:frame].size;
    return {(float) content.width, (float) content.height};
}

NSSize frameSizeForContentSize(NSWindow* window, Point content)
{
    auto contentRect = NSMakeRect(0, 0, content.x, content.y);
    return [window frameRectForContentRect:contentRect].size;
}

// A user drag. AppKit says what size, not which edge, so the edge is read off
// which dimension moved - measured from where the drag STARTED, not from the
// size the window holds now. AppKit proposes each size from the start frame
// plus the cursor's travel, so against the start an edge drag moves one
// dimension for the whole drag. Against the current size it does not: the
// constraint moves the other dimension, the next proposal then differs in
// both, the axis flips, the constraint moves it back, and the window
// flickers between the two shapes on every mouse move.
NSSize windowWillResize(id self, SEL, NSWindow* sender, NSSize frameSize)
{
    auto* state = getDelegateState(self);
    auto proposed = contentSizeForFrameSize(sender, frameSize);

    if (!state->liveResizeStart)
        state->liveResizeStart = currentContentSize(sender);

    auto axis = resizeAxisBetween(*state->liveResizeStart, proposed);
    auto allowed = state->sizeConstraint({proposed, axis});
    return frameSizeForContentSize(sender, allowed);
}

void windowWillStartLiveResize(id self, SEL, NSNotification* notification)
{
    auto* window = (NSWindow*) notification.object;
    getDelegateState(self)->liveResizeStart = currentContentSize(window);
}

void windowDidEndLiveResize(id self, SEL, NSNotification*)
{
    getDelegateState(self)->liveResizeStart.reset();
}

// The green button's zoom. Never passes through windowWillResize, so it is
// the one drag-free shape the constraint would otherwise miss: the largest
// allowed size that fits the screen's default frame, kept to its top-left.
NSRect windowWillUseStandardFrame(id self, SEL, NSWindow* sender, NSRect defaultFrame)
{
    auto* state = getDelegateState(self);
    auto available = contentSizeForFrameSize(sender, defaultFrame.size);
    auto allowed = fitWithin(state->sizeConstraint, available);
    auto size = frameSizeForContentSize(sender, allowed);

    return NSMakeRect(defaultFrame.origin.x,
                      NSMaxY(defaultFrame) - size.height,
                      size.width,
                      size.height);
}

// Fullscreen hands the window the display; a content size smaller than that
// is centred on black, which is the letterbox a constrained window wants.
NSSize windowWillUseFullScreenContentSize(id self,
                                          SEL,
                                          NSWindow*,
                                          NSSize proposedSize)
{
    auto* state = getDelegateState(self);
    auto available = Point {(float) proposedSize.width, (float) proposedSize.height};
    auto allowed = fitWithin(state->sizeConstraint, available);
    return NSMakeSize(allowed.x, allowed.y);
}

void windowDidResize(id self, SEL, NSNotification* notification)
{
    auto* state = getDelegateState(self);
    auto* window = (NSWindow*) notification.object;

    if (state->keepTrafficLightsPositioned)
        repositionTrafficLights(window, state->trafficLightInset);

    if (!state->onResize)
        return;

    auto content = [window contentRectForFrameRect:[window frame]];
    state->onResize((int) content.size.width, (int) content.size.height);
}

void windowDidMove(id self, SEL, NSNotification* notification)
{
    auto* state = getDelegateState(self);

    if (state->events == nullptr)
        return;

    state->events->onMoved(toScreenPoint([(NSWindow*) notification.object frame]));
}

void notifyKeyState(id self, bool isKey)
{
    auto* state = getDelegateState(self);

    if (state->onKeyStateChanged)
        state->onKeyStateChanged(isKey);

    if (state->events != nullptr && state->events->onActivationChanged)
        state->events->onActivationChanged(isKey);
}

void windowDidBecomeKey(id self, SEL, NSNotification*)
{
    notifyKeyState(self, true);
}

void windowDidResignKey(id self, SEL, NSNotification*)
{
    notifyKeyState(self, false);
}

void deallocDelegate(id self, SEL)
{
    delete getDelegateState(self);
    ObjC::sendSuper<void>(self, [NSObject class], @selector(dealloc));
}

Class getWindowDelegateClass()
{
    static auto instance = []
    {
        auto builder =
            new ObjC::RuntimeClass<NSObject>("EacpWindowDelegateBridge");

        builder->addIvar<void*>("state");
        builder->addProtocol(@protocol(NSWindowDelegate));

        builder->addMethod(@selector(windowWillClose:), windowWillClose);
        builder->addMethod(@selector(windowShouldClose:), windowShouldClose);
        builder->addMethod(@selector(windowWillResize:toSize:),
                           windowWillResize);
        builder->addMethod(@selector(windowWillStartLiveResize:),
                           windowWillStartLiveResize);
        builder->addMethod(@selector(windowDidEndLiveResize:),
                           windowDidEndLiveResize);
        builder->addMethod(@selector(windowWillUseStandardFrame:defaultFrame:),
                           windowWillUseStandardFrame);
        builder->addMethod(@selector(window:willUseFullScreenContentSize:),
                           windowWillUseFullScreenContentSize);
        builder->addMethod(@selector(windowDidResize:), windowDidResize);
        builder->addMethod(@selector(windowDidMove:), windowDidMove);
        builder->addMethod(@selector(windowDidBecomeKey:), windowDidBecomeKey);
        builder->addMethod(@selector(windowDidResignKey:), windowDidResignKey);
        builder->addMethod(@selector(dealloc), deallocDelegate);

        builder->registerClass();
        return builder;
    }();

    return instance->get();
}

// The state the dismissal blocks hold, weakly. Its owner is the popup's
// Native, so a block that outlives the window — one already queued when the
// app destroyed the popup from the handler — finds nothing and does nothing.
struct PopupDismissal
{
    WindowEvents* events = nullptr;
    bool dismissOnOutsideClick = true;

    // One request per showing. A click into another app is reported twice —
    // by the global monitor and by the app deactivating — and a held Escape
    // repeats, so without the latch a handler that only hides the popup is
    // called again and again for the one gesture that closed it.
    bool requested = false;
};

using PopupDismissalRef = std::shared_ptr<PopupDismissal>;

// Deferred on purpose: the handler's whole job is to destroy or hide the
// window whose monitor block is running, and unwinding an NSEvent monitor
// through a freed NSWindow is not a thing to try.
void requestPopupDismissal(const std::weak_ptr<PopupDismissal>& state)
{
    auto dismissal = state.lock();

    if (dismissal == nullptr || dismissal->requested)
        return;

    dismissal->requested = true;

    auto deliver = [state]
    {
        auto delivered = state.lock();

        if (delivered == nullptr)
            return;

        // Copied before it runs: the handler is expected to destroy the
        // Window, and the handler is a member of it — invoking it in place
        // would free the callable half way through its own call.
        auto handler = delivered->events->onDismissRequested;
        handler();
    };

    Threads::callAsync(deliver);
}

CGPoint screenLocationOf(NSEvent* event)
{
    if (event.window != nil)
        return [event.window convertPointToScreen:event.locationInWindow];

    // An event with no window carries its location in screen coordinates
    // already — which is what a press in another app's window arrives as.
    return event.locationInWindow;
}

// Whether `window` is `ancestor` or one of the child windows hanging off it,
// however deep — a popup's own submenu, and the submenu's submenu.
bool isWithinWindow(NSWindow* window, NSWindow* ancestor)
{
    for (NSWindow* current = window; current != nil; current = current.parentWindow)
        if (current == ancestor)
            return true;

    return false;
}

// Everything that watches over a child window: the event monitors that ask a
// popup to dismiss — the local one that swallows a press landing outside it,
// the global one for a press in another app, which cannot be swallowed since
// it is not ours — and the owner's notifications.
//
// The monitors come and go with the popup being on screen; the observers stay
// for as long as the window does, because an owner closing while the popup is
// hidden still has to hand the child back before it goes.
class PopupWatcher
{
public:
    PopupWatcher() = default;
    ~PopupWatcher() { remove(); }

    // It owns monitors and observers it removes by hand, so a copy would
    // remove them twice and a move would leave the source removing them early.
    PopupWatcher(const PopupWatcher&) = delete;
    PopupWatcher& operator=(const PopupWatcher&) = delete;
    PopupWatcher(PopupWatcher&&) = delete;
    PopupWatcher& operator=(PopupWatcher&&) = delete;

    // A plain child window passes no state: it wants the detach and none of
    // the dismissal.
    void install(NSWindow* childToUse,
                 NSWindow* owner,
                 const PopupDismissalRef& state)
    {
        remove();

        child = childToUse;
        dismissal = state;

        installOwnerObservers(owner);
        installEventMonitors();
    }

    void installEventMonitors()
    {
        if (dismissal == nullptr || !monitors.empty())
            return;

        installPressAndKeyMonitors();
    }

    void removeEventMonitors()
    {
        for (auto& monitor: monitors)
            [NSEvent removeMonitor:monitor.get()];

        monitors.clear();
    }

    void remove()
    {
        removeEventMonitors();

        auto* center = NSNotificationCenter.defaultCenter;

        for (auto& observer: observers)
            [center removeObserver:observer.get()];

        observers.clear();

        child = nil;
        dismissal.reset();
    }

private:
    void installPressAndKeyMonitors()
    {
        auto weakState = std::weak_ptr<PopupDismissal> {dismissal};
        auto dismissOnOutsideClick = dismissal->dismissOnOutsideClick;
        auto* popup = child;

        auto presses = NSEventMaskLeftMouseDown | NSEventMaskRightMouseDown
                       | NSEventMaskOtherMouseDown;

        auto handler = ^NSEvent*(NSEvent* event)
        {
            if (event.type == NSEventTypeKeyDown)
            {
                if (event.keyCode != KeyCode::Escape)
                    return event;

                requestPopupDismissal(weakState);
                return nil;
            }

            if (event.window == popup)
                return event;

            // Another popup of ours: a submenu of this one, or a menu opened
            // from somewhere else entirely. Never swallowed either way — the
            // press belongs to the window it landed in, and a menu that ate
            // its own submenu's clicks would leave the submenu unusable.
            // Only the unrelated one takes this menu down with it.
            if (isPopupWindow(event.window))
            {
                if (!isWithinWindow(event.window, popup) && dismissOnOutsideClick)
                    requestPopupDismissal(weakState);

                return event;
            }

            // Geometry is for the presses that name no window at all, which
            // is what a press in another application arrives as.
            if (NSPointInRect(screenLocationOf(event), popup.frame))
                return event;

            if (!dismissOnOutsideClick)
                return event;

            requestPopupDismissal(weakState);

            // Swallowed: the press that closes a menu belongs to the menu,
            // which is why clicking one away never also presses what was
            // under it.
            return nil;
        };

        id local = [NSEvent
            addLocalMonitorForEventsMatchingMask:presses | NSEventMaskKeyDown
                                         handler:handler];

        // A press in another app: ours to hear about and not ours to stop.
        auto globalHandler = ^(NSEvent*)
        {
            if (dismissOnOutsideClick)
                requestPopupDismissal(weakState);
        };

        id global = [NSEvent addGlobalMonitorForEventsMatchingMask:presses
                                                          handler:globalHandler];

        monitors.add(ObjC::attachPtr((NSObject*) local));
        monitors.add(ObjC::attachPtr((NSObject*) global));
    }

    void installOwnerObservers(NSWindow* owner)
    {
        auto weakState = std::weak_ptr<PopupDismissal> {dismissal};
        auto* popup = child;

        auto observe =
            [this](NSNotificationName name, id object, void (^body)(NSNotification*))
        {
            if (object == nil)
                return;

            auto* token = [NSNotificationCenter.defaultCenter
                addObserverForName:name
                            object:object
                             queue:nil
                        usingBlock:body];

            observers.add(ObjC::attachPtr((NSObject*) token));
        };

        auto dismiss = ^(NSNotification*) { requestPopupDismissal(weakState); };

        // The owner's close is also the last moment both windows are alive:
        // detach here rather than leave the popup's destructor to ask a window
        // that may be gone by then who its parent was.
        auto ownerClosing = ^(NSNotification*)
        {
            if (popup.parentWindow != nil)
                [popup.parentWindow removeChildWindow:popup];

            requestPopupDismissal(weakState);
        };

        observe(NSWindowWillCloseNotification, owner, ownerClosing);

        // The rest is the popup's alone: an ordinary child window has no
        // reason to go when its owner stops being key.
        if (dismissal == nullptr)
            return;

        observe(NSWindowDidResignKeyNotification, owner, dismiss);
        observe(NSWindowDidResignMainNotification, owner, dismiss);
        observe(NSWindowDidMiniaturizeNotification, owner, dismiss);
        observe(NSApplicationDidResignActiveNotification, NSApp, dismiss);
    }

    NSWindow* child = nil;
    PopupDismissalRef dismissal;
    Vector<ObjC::Ptr<NSObject>> monitors;
    Vector<ObjC::Ptr<NSObject>> observers;
};
} // namespace

bool isPopupWindow(NSWindow* window)
{
    return window != nil && [window isKindOfClass:getPopupPanelClass()];
}

Point screenPointFromAppKit(CGPoint appKitPoint)
{
    return {(float) appKitPoint.x, (float) (primaryScreenTop() - appKitPoint.y)};
}

CGPoint appKitPointFromScreen(Point screenPoint)
{
    return CGPointMake(screenPoint.x, primaryScreenTop() - screenPoint.y);
}

NSObject* createWindowDelegate(const WindowOptions& options)
{
    NSObject* bridge = [[getWindowDelegateClass() alloc] init];

    auto* state = new WindowDelegateState();
    state->cb = options.effectiveOnQuit();
    state->hidesOnClose = options.hidesOnClose;
    state->onResize = options.onResize;
    state->sizeConstraint = options.effectiveSizeConstraint();
    state->keepTrafficLightsPositioned =
        options.trafficLightPosition.has_value();

    if (options.trafficLightPosition)
        state->trafficLightInset = NSMakePoint(options.trafficLightPosition->x,
                                               options.trafficLightPosition->y);

    ObjC::getIvar<void*>(bridge, "state") = state;
    return bridge;
}

NSWindowStyleMask getFlag(WindowFlags flag)
{
    switch (flag)
    {
        case WindowFlags::Borderless:
            return NSWindowStyleMaskBorderless;
        case WindowFlags::Titled:
            return NSWindowStyleMaskTitled;
        case WindowFlags::Closable:
            return NSWindowStyleMaskClosable;
        case WindowFlags::Miniaturizable:
            return NSWindowStyleMaskMiniaturizable;
        case WindowFlags::Resizable:
            return NSWindowStyleMaskResizable;
        case WindowFlags::UnifiedTitleAndToolbar:
            return NSWindowStyleMaskUnifiedTitleAndToolbar;
        case WindowFlags::FullScreen:
            return NSWindowStyleMaskFullScreen;
        case WindowFlags::FullSizeContentView:
            return NSWindowStyleMaskFullSizeContentView;
        case WindowFlags::UtilityWindow:
            return NSWindowStyleMaskUtilityWindow;
        case WindowFlags::DocModalWindow:
            return NSWindowStyleMaskDocModalWindow;
        case WindowFlags::NonactivatingPanel:
            return NSWindowStyleMaskNonactivatingPanel;
        case WindowFlags::HUDWindow:
            return NSWindowStyleMaskHUDWindow;
    }

    return {};
}

NSWindowStyleMask getStyle(const WindowOptions& options)
{
    // A popup is what the option says it is, not what the default flags left
    // behind: borderless, and nonactivating so a click inside it leaves the
    // owner key.
    if (options.popup)
        return NSWindowStyleMaskBorderless | NSWindowStyleMaskNonactivatingPanel;

    auto res = NSWindowStyleMask();

    for (auto& flag: options.flags)
        res |= getFlag(flag);

    return res;
}

// The class has to match the mask. NSPanel is what implements the utility,
// HUD, doc-modal and nonactivating behaviours, and a plain NSWindow given one
// of those masks simply ignores it.
Class windowClassFor(const WindowOptions& options, NSWindowStyleMask style)
{
    if (options.popup)
        return getPopupPanelClass();

    if ((style & panelOnlyStyleMask) != 0)
        return [NSPanel class];

    // NSWindowStyleMaskBorderless is 0 — "borderless" is the absence of the
    // Titled bit, so that's what selects the keyable subclass.
    return (style & NSWindowStyleMaskTitled) != 0
               ? [NSWindow class]
               : getKeyableBorderlessWindowClass();
}

// The window a popup is owned by: the eacp one if it was given, else whatever
// the host handed over — an NSWindow, or a view inside one, which is the form
// a plugin API names.
NSWindow* resolveOwnerWindow(const WindowOptions& options)
{
    if (options.parent != nullptr)
        return (NSWindow*) options.parent->getHandle();

    id nativeParent = (id) options.nativeParent;

    if ([nativeParent isKindOfClass:[NSView class]])
        return [(NSView*) nativeParent window];

    if ([nativeParent isKindOfClass:[NSWindow class]])
        return (NSWindow*) nativeParent;

    return nil;
}

struct Window::Native
{
    Native(const WindowOptions& options, WindowEvents& eventsToUse)
        : opts(options)
    {
        auto style = getStyle(options);
        auto initialSize = options.effectiveInitialSize();
        auto contentRect = NSMakeRect(0, 0, initialSize.x, initialSize.y);
        auto windowClass = windowClassFor(options, style);

        handle = [[windowClass alloc] initWithContentRect:contentRect
                                                styleMask:style
                                                  backing:NSBackingStoreBuffered
                                                    defer:NO];

        delegate = createWindowDelegate(options);
        getDelegateState(delegate.get())->events = &eventsToUse;
        getDelegateState(delegate.get())->onKeyStateChanged = [this](bool isKey)
        {
            keyStateChanged(isKey);

            // Becoming key is the moment to hand keyboard focus to the content.
            // AppKit would otherwise park first responder on the content view
            // itself; for a WebView that is the empty container, leaving the
            // page unfocused until clicked. Re-run on every activation so focus
            // is restored after a sibling window (settings / keyboard) took it.
            if (isKey)
                focusContentView();
        };

        [getWindow() setRestorable:NO];
        [getWindow() setReleasedWhenClosed:NO];
        [getWindow() setTitle:@(options.title.c_str())];
        [getWindow() setTitleVisibility:options.showTitle ? NSWindowTitleVisible
                                                          : NSWindowTitleHidden];
        [getWindow()
            setTitlebarAppearsTransparent:options.titlebarTransparent];
        [getWindow() setIgnoresMouseEvents:options.ignoresMouseEvents];

        if (@available(macOS 11.0, *))
        {
            [getWindow() setTitlebarSeparatorStyle:
                             options.showTitlebarSeparator
                                 ? NSTitlebarSeparatorStyleAutomatic
                                 : NSTitlebarSeparatorStyleNone];
        }

        if (options.backgroundColor)
        {
            const auto& c = *options.backgroundColor;
            [getWindow() setBackgroundColor:[NSColor colorWithSRGBRed:c.r
                                                                green:c.g
                                                                 blue:c.b
                                                                alpha:c.a]];
        }

        // An opaque window paints its background square into the corners, and
        // over everything a see-through content view was meant to reveal. Make
        // the window itself clear and let the content — clipped to the radius
        // in setContentView — define the visible shape; the shadow follows it
        // automatically. This wins over backgroundColor by design; see
        // WindowOptions.
        if (options.cornerRadius || options.transparentBackground)
        {
            [getWindow() setOpaque:NO];
            [getWindow() setBackgroundColor:[NSColor clearColor]];
        }

        if (options.minWidth > 0 || options.minHeight > 0)
            [getWindow() setContentMinSize:NSMakeSize(options.minWidth,
                                                      options.minHeight)];

        if (options.alwaysOnTop)
            [getWindow() setLevel:NSFloatingWindowLevel];

        applyCollectionBehavior();

        if (options.initialPosition)
        {
            // initialPosition is top-left from the primary display's
            // top-left; see appKitPointFromScreen for the flip.
            [getWindow() setFrameTopLeftPoint:appKitPointFromScreen(
                                                  *options.initialPosition)];
        }
        else
        {
            [getWindow() center];
        }

        if (options.popup)
            containPopupWithinVisibleFrame();
        else
            containWithinVisibleFrame(options);

        [getWindow() setDelegate:(id<NSWindowDelegate>) delegate.get()];

        attachToOwner(eventsToUse);

        if (options.effectiveShowInactive())
        {
            if (!eacp::Apps::getAppEnvironment().headless)
                [getWindow() orderFront:nil];
        }
        else
        {
            toFront();
        }

        if (options.trafficLightPosition)
            repositionTrafficLights(
                getWindow(),
                NSMakePoint(options.trafficLightPosition->x,
                            options.trafficLightPosition->y));

        // A popup is a piece of another window, not one of the app's own, so
        // it has nothing to say about the Dock tile.
        if (!options.popup)
            applyApplicationIcon(options.applicationIcon());
    }

    // The popup half of the constructor: the panel behaviours that only exist
    // on NSPanel, the ownership that makes the thing travel with the window it
    // pops over, and the watchers that ask for it to be dismissed.
    void attachToOwner(WindowEvents& eventsToUse)
    {
        auto* owner = resolveOwnerWindow(opts);

        if (opts.popup)
            applyPopupBehaviour();

        if (owner != nil)
        {
            // Ordered directly above its owner, moved with it and ordered out
            // with it — all of which AppKit does for a child window, and none
            // of which an app tracking onMoved by hand does as smoothly.
            [owner addChildWindow:getWindow() ordered:NSWindowAbove];

            // A child window inherits its parent's level, so the menu level
            // goes on after the adoption or it is quietly dropped — and a
            // popup that shares its owner's level is one a foreign native
            // child view can still cover.
            if (opts.popup)
                [getWindow() setLevel:NSPopUpMenuWindowLevel];

            // The press that opened the popup is over as far as the owner is
            // concerned: the view it went down in must not sit waiting for an
            // up that now belongs to the menu.
            if (opts.popup && opts.parent != nullptr)
                if (auto* content = opts.parent->contentLink.contentView)
                    content->cancelMouseCapture();
        }

        if (opts.popup)
        {
            dismissal = std::make_shared<PopupDismissal>();
            dismissal->events = &eventsToUse;
            dismissal->dismissOnOutsideClick = opts.dismissOnOutsideClick;
        }

        // A plain child window watches too, for the one notification that
        // keeps its destructor from asking a freed owner about its children.
        if (owner != nil || dismissal != nullptr)
            watcher.install(getWindow(), owner, dismissal);
    }

    void applyPopupBehaviour()
    {
        auto* panel = (NSPanel*) getWindow();

        // A menu stays up while the app is switched away from — the dismissal
        // is ours to decide, through onDismissRequested, not AppKit's to make
        // by ordering the window out behind our back.
        [panel setHidesOnDeactivate:NO];
        [panel setBecomesKeyOnlyIfNeeded:YES];
        [panel setWorksWhenModal:YES];
        [panel setHasShadow:YES];
        [panel setAcceptsMouseMovedEvents:YES];
        [panel setLevel:NSPopUpMenuWindowLevel];
        [panel setAnimationBehavior:NSWindowAnimationBehaviorUtilityWindow];
    }

    // A menu opened near an edge slides back onto the display rather than
    // being recentred the way a document window is: where it opened is where
    // the click was, and moving it any further than it has to be moved loses
    // that.
    void containPopupWithinVisibleFrame()
    {
        NSWindow* window = getWindow();
        NSScreen* screen = window.screen != nil ? window.screen
                                                : NSScreen.mainScreen;

        if (screen == nil)
            return;

        auto visible = screen.visibleFrame;
        auto frame = window.frame;

        frame.origin.x = std::min(frame.origin.x,
                                  NSMaxX(visible) - frame.size.width);
        frame.origin.x = std::max(frame.origin.x, NSMinX(visible));
        frame.origin.y = std::max(frame.origin.y, NSMinY(visible));
        frame.origin.y = std::min(frame.origin.y,
                                  NSMaxY(visible) - frame.size.height);

        [window setFrame:frame display:NO];
    }

    // Whatever size was asked for, the window that opens is one the user can
    // reach all of.
    //
    // A window is sized for the display it was designed on, and 1360x860 is
    // wider than a 13" laptop's whole screen — so it opens with its bottom
    // right past the edge, and what is out there is the resize corner: the way
    // out of the shape is the part that went missing with it. AppKit's own
    // constraint keeps the title bar reachable and the height within the
    // screen; the width it leaves alone.
    //
    // Only windows that do not already fit are touched, so the placement
    // AppKit chose for every window that does is left exactly as it was.
    void containWithinVisibleFrame(const WindowOptions& options)
    {
        NSWindow* window = getWindow();
        NSScreen* screen = window.screen != nil ? window.screen
                                                : NSScreen.mainScreen;

        if (screen == nil)
            return;

        auto visible = screen.visibleFrame;
        auto frame = window.frame;

        if (NSContainsRect(visible, frame))
            return;

        // Trimming the sides independently would hand a constrained window a
        // shape it exists to refuse, so the trimmed size goes back through
        // the constraint.
        auto available = contentSizeForFrameSize(
            window,
            NSMakeSize(std::min(frame.size.width, visible.size.width),
                       std::min(frame.size.height, visible.size.height)));
        auto size = frameSizeForContentSize(
            window, fitWithin(options.effectiveSizeConstraint(), available));
        auto width = size.width;
        auto height = size.height;

        frame = NSMakeRect(NSMinX(visible) + (visible.size.width - width) / 2.0,
                           NSMinY(visible) + (visible.size.height - height) / 2.0,
                           width,
                           height);

        // Before the frame, or AppKit clamps the window straight back up to a
        // minimum that is itself bigger than the screen — a floor the user
        // cannot escape, since every drag back into shape is refused by the
        // same constraint that put the window off the edge.
        auto minSize = window.contentMinSize;
        auto content = [window contentRectForFrameRect:frame].size;
        [window setContentMinSize:NSMakeSize(std::min(minSize.width,
                                                      content.width),
                                             std::min(minSize.height,
                                                      content.height))];

        [window setFrame:frame display:NO];
    }

    // macOS has no per-window icons; the icon is the app's Dock tile,
    // shared by every window. An invalid image leaves the bundle's .icns
    // showing — the same icon Finder uses at rest — so this only fires for
    // dynamic runtime icons. When neither exists, say so: a silently
    // generic Dock tile otherwise looks like a rendering bug.
    static void applyApplicationIcon(const Image& image)
    {
        if (auto* icon = toNSImage(image))
        {
            [NSApp setApplicationIconImage:icon];
            return;
        }

        if (eacp::Apps::getAppEnvironment().headless)
            return;

        NSString* iconFile = [NSBundle.mainBundle
            objectForInfoDictionaryKey:@"CFBundleIconFile"];

        if (iconFile.length == 0)
            LOG("This app has no icon: set one with eacp_set_app_icon in "
                "CMake, or provide WindowOptions::applicationIcon for a "
                "dynamic one. The Dock and Finder show the generic icon.");
    }

    void toFront()
    {
        if (eacp::Apps::getAppEnvironment().headless)
            return;

        [getWindow() makeKeyAndOrderFront:nil];

        // While activation is pending (see ensureAppBecomesActive), still
        // show the window above other apps' windows — visible immediately,
        // and a click into it completes the activation. Raised once, not on
        // the retries: re-raising would fight the user's window arrangement.
        if (! NSApp.active)
            [getWindow() orderFrontRegardless];

        ensureAppBecomesActive();
    }

    void setTitle(const std::string& title)
    {
        [getWindow() setTitle:@(title.c_str())];
    }

    void setContentView(View& view)
    {
        contentView = &view;

        auto v = (NSView*) view.getHandle();
        [getWindow() setContentView:v];
        [v setAutoresizingMask:NSViewWidthSizable | NSViewHeightSizable];

        // Content set after the window is already key (e.g. shown, then
        // populated) misses windowDidBecomeKey, so focus the target now.
        if ([getWindow() isKeyWindow])
            focusContentView();

        if (opts.cornerRadius)
        {
            // Pairs with the clear window background set in the ctor: the
            // rounded, clipped content view is what defines the window's
            // visible shape.
            v.wantsLayer = YES;
            v.layer.cornerRadius = *opts.cornerRadius;
            v.layer.masksToBounds = YES;
        }
    }

    // The Spaces and fullscreen behaviours share one mask, so they are set
    // together, on top of whatever the window class already carries.
    void applyCollectionBehavior()
    {
        auto behavior = [getWindow() collectionBehavior];

        // FullScreenAuxiliary is already a denial: it is the behaviour of a
        // palette riding along with someone else's fullscreen, which is not a
        // window that takes the screen itself. So it doubles as the opt-out,
        // and dropping it for FullScreenNone would cost the Spaces behaviour
        // to buy something it already has.
        if (opts.visibleOnAllWorkspaces)
        {
            behavior |= NSWindowCollectionBehaviorCanJoinAllSpaces
                        | NSWindowCollectionBehaviorFullScreenAuxiliary;
        }
        else if (!opts.effectiveAllowsFullScreen())
        {
            // The three fullscreen behaviours are one either/or slot, so the
            // opt-out replaces whatever is sitting in it.
            behavior &= ~(NSWindowCollectionBehaviorFullScreenPrimary
                          | NSWindowCollectionBehaviorFullScreenAuxiliary);
            behavior |= NSWindowCollectionBehaviorFullScreenNone;
        }

        [getWindow() setCollectionBehavior:behavior];
    }

    void setVisible(bool visible)
    {
        if (eacp::Apps::getAppEnvironment().headless)
            return;

        // The contentView.hidden toggle is for WKWebView's benefit: WebKit
        // gates a page's timers, rAF and painting on view visibility, and
        // for ordered-out windows it relies on occlusion notifications that
        // don't always re-fire on a plain orderFront of a non-key window.
        // Explicitly hiding/unhiding the content view makes the transition
        // unambiguous, so a re-shown page reliably wakes back up.
        if (!visible)
        {
            // Nothing watches for an outside click while there is nothing on
            // screen to click outside of. The owner's observers stay: a hidden
            // popup is still a child window its owner has to hand back.
            watcher.removeEventMonitors();

            [getWindow() orderOut:nil];
            getWindow().contentView.hidden = YES;
            return;
        }

        getWindow().contentView.hidden = NO;

        // A new showing is a new dismissal to ask for: the one the popup was
        // hidden over is spent.
        if (dismissal != nullptr)
            dismissal->requested = false;

        watcher.installEventMonitors();

        // Re-assert the float level + Spaces behaviour on every show —
        // cheap, and guards against anything having knocked them off while
        // the window was ordered out.
        if (opts.alwaysOnTop)
            [getWindow() setLevel:NSFloatingWindowLevel];

        // A child window takes its owner's level when it is ordered back in,
        // so the menu level has to be said again — otherwise a popup that was
        // hidden and shown comes back level with the window it exists to
        // cover, and the foreign native content in that window covers it.
        if (opts.popup)
            [getWindow() setLevel:NSPopUpMenuWindowLevel];

        applyCollectionBehavior();

        if (opts.effectiveShowInactive())
            [getWindow() orderFront:nil];
        else
            [getWindow() makeKeyAndOrderFront:nil];
    }

    void minimize()
    {
        if (eacp::Apps::getAppEnvironment().headless)
            return;

        [getWindow() miniaturize:nil];
    }

    void toggleMaximize()
    {
        if (eacp::Apps::getAppEnvironment().headless)
            return;

        // zoom: is itself a toggle — it restores the saved frame when the
        // window is already zoomed, matching the Windows caption button.
        [getWindow() zoom:nil];
    }

    NSWindow* getWindow() { return handle.get(); }

    // Objective-C has no const-qualified message send, so a const method
    // reading geometry off the window has to drop the qualifier the C++ side
    // put on the pointer. -frame mutates nothing.
    NSWindow* getWindow() const { return const_cast<NSWindow*>(handle.get()); }

    void setMouseLocked(bool locked)
    {
        if (mouseLockIntent == locked)
            return;

        mouseLockIntent = locked;

        if (locked && [getWindow() isKeyWindow])
            engageMouseLock();
        else if (!locked)
            disengageMouseLock();
    }

    void focusContentView()
    {
        if (contentView == nullptr)
            return;

        auto* target = (NSView*) contentView->nativeFocusTarget();
        if (target == nil)
            return;

        // Leave focus alone when it already lives inside the target (e.g. a
        // text field the user is editing), so re-activating doesn't blur it.
        id current = [getWindow() firstResponder];
        if ([current isKindOfClass:[NSView class]]
            && [(NSView*) current isDescendantOf:target])
            return;

        [getWindow() makeFirstResponder:target];
    }

    void keyStateChanged(bool isKey)
    {
        if (!mouseLockIntent)
            return;

        if (isKey)
            engageMouseLock();
        else
            disengageMouseLock();
    }

    void engageMouseLock()
    {
        if (mouseLockEngaged)
            return;

        mouseLockEngaged = true;
        [getWindow() setAcceptsMouseMovedEvents:YES];
        CGAssociateMouseAndMouseCursorPosition(false);
        warpCursorToWindowCenter();
        [NSCursor hide];
    }

    void disengageMouseLock()
    {
        if (!mouseLockEngaged)
            return;

        mouseLockEngaged = false;
        CGAssociateMouseAndMouseCursorPosition(true);
        [NSCursor unhide];
    }

    void warpCursorToWindowCenter()
    {
        auto content =
            [getWindow() contentRectForFrameRect:[getWindow() frame]];
        auto center = NSMakePoint(NSMidX(content), NSMidY(content));

        // AppKit screen coordinates have their origin at the primary
        // screen's bottom-left; the CG warp wants top-left.
        auto primaryHeight = NSMaxY([[NSScreen screens] firstObject].frame);
        CGWarpMouseCursorPosition(
            CGPointMake(center.x, primaryHeight - center.y));

        // The warp is not motion, but the next mouse event reports it as
        // though it were: it carries the whole jump as its delta. Left alone
        // that arrives as one huge movement and spins a locked camera round.
        detail::cursorWasWarped = true;
    }

    ~Native()
    {
        disengageMouseLock();

        // Before the window goes: the monitor blocks hold it, and a dismissal
        // already queued must find the state gone rather than the window.
        watcher.remove();
        dismissal.reset();

        // The owner keeps a list of its children and AppKit keeps the other
        // half of it on the child, so asking the window itself copes with an
        // owner that closed first — a closed parent drops its children on the
        // way out and leaves parentWindow nil.
        if (auto* owner = handle.get().parentWindow)
            [owner removeChildWindow:handle.get()];

        // Mirror Window-Windows.cpp's WM_DESTROY: programmatic destruction
        // must not fire the quit callback — only a user-initiated close may.
        // The delegate's windowWillClose: would invoke it during [close], so
        // detach the delegate first.
        [handle.get() setDelegate:nil];
        [handle.get() close];
    }

    WindowOptions opts;
    ObjC::Ptr<NSWindow> handle;
    ObjC::Ptr<NSObject> delegate;
    View* contentView = nullptr;
    bool mouseLockIntent = false;
    bool mouseLockEngaged = false;

    // Popups only: null in every other window.
    PopupDismissalRef dismissal;
    PopupWatcher watcher;
};

Window::Window(const WindowOptions& optionsToUse)
    : options(optionsToUse)
    , impl(options, events)
{
}

void Window::setTitle(const std::string& title)
{
    impl->setTitle(title);
}

void Window::setContentView(View& view)
{
    contentLink.attach(&view, this);
    impl->setContentView(view);
}

void Window::toFront()
{
    impl->toFront();
}

void Window::setVisible(bool visible)
{
    impl->setVisible(visible);
}

bool Window::isVisible()
{
    return [impl->getWindow() isVisible];
}

void Window::minimize()
{
    impl->minimize();
}

void Window::toggleMaximize()
{
    impl->toggleMaximize();
}

void* Window::getHandle()
{
    return impl->getWindow();
}

void* Window::getContentViewHandle()
{
    return [impl->getWindow() contentView];
}

Window::~Window() = default;

void Window::setMouseLocked(bool locked)
{
    impl->setMouseLocked(locked);
}

Point Window::getPosition() const
{
    return toScreenPoint([impl->getWindow() frame]);
}

void Window::setPosition(Point position)
{
    [impl->getWindow() setFrameTopLeftPoint:appKitPointFromScreen(position)];
}

Point Window::getSize() const
{
    return currentContentSize(impl->getWindow());
}

// setContentSize keeps the bottom-left, which on a screen with y growing up
// is the corner the user does not think of as anchored; the frame is
// recomputed by hand so the top-left holds instead.
void Window::setSize(Point size)
{
    NSWindow* window = impl->getWindow();
    auto frame = window.frame;
    auto newSize =
        frameSizeForContentSize(window, options.effectiveSize(size));

    frame.origin.y = NSMaxY(frame) - newSize.height;
    frame.size = newSize;

    [window setFrame:frame display:YES];
}

bool Window::isMouseLocked() const
{
    return impl->mouseLockIntent;
}

bool Window::isKeyPressed(uint16_t virtualKeyCode) const
{
    return Keyboard::isKeyPressed(virtualKeyCode);
}

bool Window::isShiftPressed() const
{
    return Keyboard::isShiftPressed();
}

bool Window::isControlPressed() const
{
    return Keyboard::isControlPressed();
}

bool Window::isAltPressed() const
{
    return Keyboard::isAltPressed();
}

bool Window::isCommandPressed() const
{
    return Keyboard::isCommandPressed();
}

ModifierKeys Window::getModifiers() const
{
    return Keyboard::getModifiers();
}

} // namespace eacp::Graphics
