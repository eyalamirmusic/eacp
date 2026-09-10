#include "Window.h"
#include "MouseLock-macOS.h"
#include "../Graphics/Keyboard.h"
#include "../Helpers/ImageConversion-macOS.h"
#include "../Primitives/GraphicUtils.h"
#include <eacp/Core/ObjC/RuntimeClass.h>
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
    return {(float) frame.origin.x, (float) (primaryScreenTop() - NSMaxY(frame))};
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
} // namespace

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
    auto res = NSWindowStyleMask();

    for (auto& flag: options.flags)
        res |= getFlag(flag);

    return res;
}

struct Window::Native
{
    Native(const WindowOptions& options, WindowEvents& eventsToUse)
        : opts(options)
    {
        auto style = getStyle(options);
        auto initialSize = options.effectiveInitialSize();
        auto contentRect = NSMakeRect(0, 0, initialSize.x, initialSize.y);

        // NSWindowStyleMaskBorderless is 0 — "borderless" is the absence of
        // the Titled bit, so that's what selects the keyable subclass.
        auto windowClass = (style & NSWindowStyleMaskTitled) != 0
                               ? [NSWindow class]
                               : getKeyableBorderlessWindowClass();

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
            // top-left; see primaryScreenTop for the flip.
            [getWindow()
                setFrameTopLeftPoint:NSMakePoint(options.initialPosition->x,
                                                 primaryScreenTop()
                                                     - options.initialPosition
                                                           ->y)];
        }
        else
        {
            [getWindow() center];
        }

        containWithinVisibleFrame(options);

        [getWindow() setDelegate:(id<NSWindowDelegate>) delegate.get()];

        if (options.showInactive)
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

        applyApplicationIcon(options.applicationIcon());
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
            [getWindow() orderOut:nil];
            getWindow().contentView.hidden = YES;
            return;
        }

        getWindow().contentView.hidden = NO;

        // Re-assert the float level + Spaces behaviour on every show —
        // cheap, and guards against anything having knocked them off while
        // the window was ordered out.
        if (opts.alwaysOnTop)
            [getWindow() setLevel:NSFloatingWindowLevel];

        applyCollectionBehavior();

        if (opts.showInactive)
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
    [impl->getWindow()
        setFrameTopLeftPoint:NSMakePoint(position.x,
                                         primaryScreenTop() - position.y)];
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
