#include "Window.h"
#include "MouseLock-macOS.h"
#include "../Graphics/Keyboard.h"
#include "../Helpers/ImageConversion-macOS.h"
#include "../Primitives/GraphicUtils.h"
#include <eacp/Core/ObjC/RuntimeClass.h>
#import <Cocoa/Cocoa.h>

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

void requestCooperativeActivation()
{
    if (@available(macOS 14.0, *))
        [NSApp activate];
    else
        [NSApp activateIgnoringOtherApps:YES];
}

// Ask LaunchServices to "open" this app. An open of an already-running app is
// a user-level activation the system honours even while another app is
// receiving input — unlike the cooperative requests below, which measurably
// stay denied for our apps once refused at launch. Bundled apps only:
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

// Bring the app to the foreground. Activation is COOPERATIVE since macOS 14:
// activateIgnoringOtherApps: is deprecated and demoted to a plain -activate,
// which the system declines while the user is actively working in another
// app — exactly the launched-from-a-terminal / IDE case. The cooperative
// request wins instantly when the user is idle, so try it first; if it is
// still being denied a second in, escalate to the LaunchServices re-open,
// which restores the pre-macOS-14 launch-to-front behaviour. One shared
// poller — toFront() is called once per window at startup, and overlapping
// retry chains would just spam the denial.
void ensureAppBecomesActive()
{
    static auto polling = false;
    if (polling || NSApp.active)
        return;

    requestCooperativeActivation();
    polling = true;

    __block auto attempt = 0;
    [NSTimer scheduledTimerWithTimeInterval:0.25
                                    repeats:YES
                                      block:^(NSTimer* timer)
                                      {
                                          ++attempt;
                                          if (NSApp.active || attempt > 12)
                                          {
                                              polling = false;
                                              [timer invalidate];
                                              return;
                                          }

                                          if (attempt == 4)
                                              reopenSelfViaLaunchServices();
                                          else
                                              requestCooperativeActivation();
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

// Runtime classes get no automatic C++ ivar construction, so the delegate's
// C++ state lives behind one raw pointer, created with the delegate and
// deleted in its dealloc.
struct WindowDelegateState
{
    Callback cb = [] {};
    ResizeCallback onResize;
    WillResizeCallback onWillResize;
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

NSSize windowWillResize(id self, SEL, NSWindow* sender, NSSize frameSize)
{
    auto* state = getDelegateState(self);

    if (!state->onWillResize)
        return frameSize;

    auto proposedFrame = NSMakeRect(0, 0, frameSize.width, frameSize.height);
    auto proposedContent = [sender contentRectForFrameRect:proposedFrame];
    auto width = (int) proposedContent.size.width;
    auto height = (int) proposedContent.size.height;
    state->onWillResize(width, height);
    proposedContent.size = NSMakeSize(width, height);
    return [sender frameRectForContentRect:proposedContent].size;
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
        builder->addMethod(@selector(windowWillResize:toSize:),
                           windowWillResize);
        builder->addMethod(@selector(windowDidResize:), windowDidResize);
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
    state->onResize = options.onResize;
    state->onWillResize = options.onWillResize;
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
        auto contentRect = NSMakeRect(0, 0, options.width, options.height);

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

        if (options.cornerRadius)
        {
            // An opaque window paints its background square into the
            // corners. Make the window itself clear and let the rounded,
            // clipped content view (see setContentView) define the visible
            // shape — the shadow follows it automatically. This wins over
            // backgroundColor by design; see WindowOptions.
            [getWindow() setOpaque:NO];
            [getWindow() setBackgroundColor:[NSColor clearColor]];
        }

        if (options.minWidth > 0 || options.minHeight > 0)
            [getWindow() setContentMinSize:NSMakeSize(options.minWidth,
                                                      options.minHeight)];

        if (options.alwaysOnTop)
            [getWindow() setLevel:NSFloatingWindowLevel];

        if (options.visibleOnAllWorkspaces)
            [getWindow()
                setCollectionBehavior:
                    NSWindowCollectionBehaviorCanJoinAllSpaces
                    | NSWindowCollectionBehaviorFullScreenAuxiliary];

        if (options.initialPosition)
        {
            // initialPosition is top-left from the primary display's top-left
            // (Electron convention); AppKit's origin is the bottom-left of
            // the primary screen, so flip y against its height.
            NSScreen* primary = NSScreen.screens.firstObject;
            auto screenTop = primary != nil ? NSMaxY(primary.frame) : 0.0;
            [getWindow()
                setFrameTopLeftPoint:NSMakePoint(options.initialPosition->x,
                                                 screenTop
                                                     - options.initialPosition
                                                           ->y)];
        }
        else
        {
            [getWindow() center];
        }

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

        if (opts.visibleOnAllWorkspaces)
            [getWindow()
                setCollectionBehavior:
                    NSWindowCollectionBehaviorCanJoinAllSpaces
                    | NSWindowCollectionBehaviorFullScreenAuxiliary];

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
