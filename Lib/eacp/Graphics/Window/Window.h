#pragma once

#include "../Image/Image.h"
#include "../Primitives/Primitives.h"
#include "../View/View.h"
#include "SizeConstraint.h"

namespace eacp::Graphics
{

enum class WindowFlags
{
    Borderless,
    Titled,
    Closable,
    Miniaturizable,
    Resizable,
    UnifiedTitleAndToolbar,
    FullScreen,
    FullSizeContentView,
    UtilityWindow,
    DocModalWindow,
    NonactivatingPanel,
    HUDWindow
};

using ResizeCallback = std::function<void(int width, int height)>;
using WillResizeCallback = std::function<void(int& width, int& height)>;

// Observable window events. Assign a handler to react; all fire on the main
// thread.
struct WindowEvents
{
    // Fires when the window gains (true) or loses (false) key focus.
    std::function<void(bool isKey)> onActivationChanged = [](bool) {};

    // Fires when the user closes a WindowOptions::hidesOnClose window: by
    // then it has ordered out, its state is intact, and the app is still
    // running. Without this the close is invisible to app code — onQuit is
    // exactly what hidesOnClose suppresses — so it is where a tray-resident
    // app drops its Dock icon (Apps::setDockIconVisible(false)) and stops
    // work that only a visible window needs. Never fires for a window that
    // closes normally; that one gets onQuit and is destroyed.
    std::function<void()> onHidden = [] {};

    // Fires after the window has moved, with its new top-left in screen
    // points — the same space WindowOptions::initialPosition and
    // Window::getPosition are in. The pair of them is what lets an app put a
    // window back where the user left it on the next launch.
    //
    // Every move, not only the user's: setPosition reports itself too. The
    // one placement it does not fire for is the window's first, from
    // initialPosition, which happens before there is a window to observe.
    //
    // Here rather than beside WindowOptions::onResize because, unlike a first
    // size, nothing needs a position before the window exists: a handler set
    // after construction has still missed nothing.
    std::function<void(Point position)> onMoved = [](auto&&) {};
};

struct WindowOptions
{
    WindowOptions()
    {
        flags.add({WindowFlags::Titled,
                   WindowFlags::Closable,
                   WindowFlags::Miniaturizable,
                   WindowFlags::Resizable});
    }

    Callback effectiveOnQuit() const
    {
        if (onQuit)
            return onQuit;
        return isPrimary ? Callback {[] { Apps::quit(); }} : Callback {[] {}};
    }

    bool effectiveAllowsFullScreen() const
    {
        return allowsFullScreen.value_or(!hasAspectRatio());
    }

    // Whether aspectRatio carries a ratio worth enforcing - both sides have to
    // be positive for it to describe a shape at all.
    bool hasAspectRatio() const
    {
        return aspectRatio && aspectRatio->x > 0.f && aspectRatio->y > 0.f;
    }

    // The one rule the platforms enforce: onWillResize, then sizeConstraint,
    // then aspectRatio - the lock last, so the shape the window ends up with
    // is the locked one however the callbacks moved the size around.
    SizeConstraint effectiveSizeConstraint() const
    {
        auto willResize = onWillResize;
        auto custom = sizeConstraint;
        auto lock = AspectRatioLock {aspectRatio.value_or(Point {})};

        return [willResize, custom, lock](const ResizeRequest& request)
        {
            auto width = (int) request.size.x;
            auto height = (int) request.size.y;
            willResize(width, height);

            auto size = custom({{(float) width, (float) height}, request.axis});
            return lock({size, request.axis});
        };
    }

    // The content size the window opens at: width/height put through the
    // rule, so a window is never made in a shape it would refuse to be
    // dragged into. Width wins where the two disagree.
    Point effectiveInitialSize() const
    {
        return effectiveSizeConstraint()(
            {{(float) width, (float) height}, ResizeAxis::Both});
    }

    // The content size a programmatic resize (Window::setSize) lands on:
    // floored at minWidth/minHeight, as a drag is before the rule sees it,
    // then through the rule as a corner resize.
    Point effectiveSize(Point size) const
    {
        auto floored = Point {std::max(size.x, (float) minWidth),
                              std::max(size.y, (float) minHeight)};

        return effectiveSizeConstraint()({floored, ResizeAxis::Both});
    }

    // When the user closes the window. If left empty, falls back to
    // Apps::quit when isPrimary is true, or a no-op otherwise.
    Callback onQuit {};

    // Set to false for secondary/popup windows so closing them doesn't
    // terminate the app when onQuit is unset.
    bool isPrimary = true;

    // Closing the window (red button / Alt+F4) hides it instead of
    // destroying it — onQuit never fires and the app keeps running with the
    // window's state alive. Bring it back with setVisible(true), e.g. from
    // a tray icon, a Dock reopen (Apps::setReopenHandler) or a notification
    // click. Quitting the app still tears the window down normally.
    bool hidesOnClose = false;

    // Called after the window has been resized. Sizes are in points and refer
    // to the content view, not the outer frame.
    //
    // For reacting to a size, not for choosing one: a resize made from here
    // fires here again. The shape a window may take is sizeConstraint's.
    ResizeCallback onResize {};

    // Called with the proposed content size, in points, before a resize is
    // applied; may be mutated to clamp it. The older, edge-blind form of
    // sizeConstraint, kept for the callers that have it: it runs first, and
    // what it leaves goes through sizeConstraint and aspectRatio.
    WillResizeCallback onWillResize = [](int&, int&) {};

    // The shapes the window may take, as a rule from the size about to be
    // applied to the one that will be (see SizeConstraint.h). Asked before
    // every size the window can take - the initial one, an edge or corner
    // drag, a maximise or zoom, fullscreen, a display too small for the
    // window - so there is no moment where the window holds a shape the rule
    // forbids, and no callback for the app to write. The default accepts
    // every size.
    //
    // AspectRatioLock is the rule an app most often wants: a ratio-locked
    // canvas, optionally under a toolbar or beside a panel that keeps its own
    // size. `sizeConstraint = AspectRatioLock {{16, 9}, {.top = 56}}` locks
    // the content below a 56-point header to 16:9; for the whole content,
    // aspectRatio below is the shorthand.
    //
    // Fullscreen and maximise give a constrained window the largest size the
    // rule allows on that display, centred: macOS and the Wayland compositor
    // letterbox the rest in black, Windows leaves it to the desktop.
    SizeConstraint sizeConstraint = [](const ResizeRequest& request)
    { return request.size; };

    int width = 640;
    int height = 400;
    std::string title = "New Window";

    // When false, the title bar still shows but the title text is hidden.
    bool showTitle = true;

    // When true, the title bar draws no background, so a FullSizeContentView's
    // content shows through beneath the traffic lights (otherwise macOS paints
    // a translucent grey band over it).
    bool titlebarTransparent = false;

    // The hairline separator drawn under the title bar. Set false to drop it
    // so a custom header blends into the content with no chrome line.
    bool showTitlebarSeparator = true;

    // macOS: inset of the window controls (close / minimise / zoom) from the
    // top-left, in points; mirrors Electron's trafficLightPosition. Only
    // meaningful with a hidden/transparent title bar (FullSizeContentView);
    // unset leaves them at the default macOS position.
    std::optional<Point> trafficLightPosition;

    // macOS: background colour shown behind the content view — before the web
    // view first paints and during live resize. Unset uses the system window
    // background; set to black to avoid a white flash on launch/resize.
    std::optional<Color> backgroundColor;

    // Nothing is painted behind the content view, so wherever the content is
    // see-through the desktop is. For floating cards and HUDs, where the page
    // draws one small shape and the surplus around it must not exist. Wins
    // over backgroundColor.
    //
    // The content then defines the whole window: macOS makes the window
    // non-opaque with a clear background, and Windows creates it with no
    // redirection surface (WS_EX_NOREDIRECTIONBITMAP) and no frame — so no
    // system rounding or shadow is drawn around the transparent surplus, and
    // cornerRadius is left to the content as well.
    bool transparentBackground = false;

    // Minimum content size in points (0 = no minimum). Content-relative, to
    // match width/height and the resize callbacks above.
    int minWidth = 0;
    int minHeight = 0;

    // Locks the whole content's proportions: the user can resize the window,
    // but only into shapes of this width-to-height ratio. Only the ratio is
    // read, so {16, 9} and {1920, 1080} mean the same thing, and a ratio with
    // a non-positive side locks nothing. The same as
    // `sizeConstraint = AspectRatioLock {ratio}`, for the case with no fixed
    // border; the two compose (see effectiveSizeConstraint) so either can be
    // set alone.
    std::optional<Point> aspectRatio;

    // Whether the user can send the window fullscreen - the green button, the
    // Window menu's Enter Full Screen, ctrl-cmd-F. Mirrors Electron's
    // fullscreenable.
    //
    // Unset allows it, EXCEPT when aspectRatio is set: a window that locks
    // its whole content is saying it has no letterbox path, so by default
    // the escape hatch goes with it, and the green button zooms instead -
    // which does respect the lock - so the window keeps a maximise gesture.
    // Set it to true to keep both; fullscreen then gives the window the
    // largest allowed size and centres it on black. A sizeConstraint alone
    // makes no such statement - a bordered lock has its own layout, and
    // lockedArea letterboxes - so it leaves fullscreen on.
    //
    // macOS only: Windows has no OS-level fullscreen mode for a window (an
    // app that wants one builds it from a borderless monitor-sized window),
    // and its maximise path honours the constraint on its own. No-op on iOS.
    std::optional<bool> allowsFullScreen;

    // Keeps the window above normal windows (macOS NSFloatingWindowLevel,
    // Windows WS_EX_TOPMOST). Mirrors Electron's alwaysOnTop.
    bool alwaysOnTop = false;

    // macOS: pins the window to every Space so it follows the user between
    // desktops and fullscreen apps. Mirrors Electron's
    // setVisibleOnAllWorkspaces(true). No-op on other platforms.
    bool visibleOnAllWorkspaces = false;

    // Shows the window without making it key / stealing focus from the
    // frontmost app (macOS orderFront vs makeKeyAndOrderFront). The window
    // can still become key when clicked. Mirrors Electron's showInactive().
    bool showInactive = false;

    // Lets mouse clicks pass through this window to whatever is underneath.
    // Useful for transient HUDs and overlays. No-op on iOS.
    bool ignoresMouseEvents = false;

    // Initial position of the window's top-left corner in screen points,
    // measured from the primary display's top-left (Electron convention).
    // Unset centers the window (macOS) / uses the system default (Windows).
    std::optional<Point> initialPosition;

    // Overrides the RUNNING app's icon with a dynamically generated one —
    // badge counts, progress overlays, theme-aware art. Called once when
    // the window is constructed: Windows stamps it on the window (title
    // bar, taskbar, Alt-Tab); macOS swaps the Dock tile.
    //
    // This is not how an app gets its icon. Finder, Explorer and a
    // not-yet-running Dock tile never execute the binary — they read the
    // static icon that eacp_set_app_icon (CMake) bakes into the bundle
    // (.icns) / executable (ICON resource). Returning an invalid Image
    // (the default) keeps that static icon, so apps whose icon never
    // changes need only the CMake call.
    std::function<Image()> applicationIcon = [] { return Image {}; };

    // Windows: overrides the icon the Alt-Tab switcher shows (the big-icon
    // slot); the title bar and taskbar keep applicationIcon. An invalid
    // Image (the default) falls back to applicationIcon, then to the
    // executable's embedded icon. No-op on macOS, which has no per-window
    // icons.
    std::function<Image()> altTabIcon = [] { return Image {}; };

    // Rounds the window's corners (points). Borderless windows are square
    // by default; set this to get the standard macOS rounded shape. On
    // macOS this makes the window non-opaque with a clear background
    // (overriding backgroundColor) so the clipped content view defines the
    // visible shape. Windows 11 rounds via DWM at the system radius — the
    // value is ignored there; earlier Windows stays square.
    std::optional<float> cornerRadius;

    EA::Vector<WindowFlags> flags;
};

struct ModifierKeys;

class Window
{
public:
    Window(const WindowOptions& optionsToUse = {});

    // The window with `view` already as its content, so a struct that pairs a
    // view with a window can write `Window window {view, options};` as a
    // member initializer and needs no constructor body for it. Declare the
    // view before the window.
    Window(View& view, const WindowOptions& optionsToUse = {});

    ~Window();

    void setTitle(const std::string& title);
    void* getHandle();
    void* getContentViewHandle();

    // Makes this view the window's content, and makes the window findable from
    // it: View::getWindow() answers for this view and everything under it.
    void setContentView(View& view);

    // Brings the window to the front and activates the app so it rises above
    // other applications. No-op under headless and on iOS.
    void toFront();

    // Shows or hides the window WITHOUT destroying it (macOS orderOut /
    // orderFront, Windows ShowWindow) — the content view and any WebView
    // state stay alive, unlike closing and recreating, and the window keeps
    // its frame, so it reappears where the user left it. Showing respects
    // showInactive (no focus steal) and re-asserts alwaysOnTop. No-op under
    // headless and on iOS.
    void setVisible(bool visible);

    // Whether the window is currently shown on screen — the query side of
    // setVisible, so toggles need no shadow bool in app code. False while
    // hidden, minimized to nothing, or under headless; always true on iOS.
    bool isVisible();

    // The window's top-left in screen points, measured from the primary
    // display's top-left and growing right and down — the same space
    // WindowOptions::initialPosition and Display report, so a position read
    // here can be stored and handed straight back to a later launch.
    //
    // The frame's top-left, title bar included, which is what
    // initialPosition places.
    //
    // A window has a frame whether or not it is on screen, so this answers
    // under headless too — where the window is made and simply never ordered
    // front. {0, 0} on iOS, which has no window to place.
    Point getPosition() const;

    // Moves the window's top-left to `position`, in the same space. Unlike
    // initialPosition this does not contain the window within the display: an
    // app moving a window on purpose is told where to put it, and a value
    // restored from disk should go through initialPosition, which does clamp.
    //
    // Fires WindowEvents::onMoved, like a move by the user does — the window
    // moved, and an app saving its position wants to hear about it however it
    // happened. No-op on iOS.
    void setPosition(Point position);

    // The content size in points - the measure WindowOptions::width/height,
    // minWidth/minHeight, onResize and sizeConstraint are all in, not the
    // outer frame. Under headless the window is made and simply never shown,
    // so this reads a real size there too. The screen on iOS.
    Point getSize() const;

    // Resizes the content to `size`, keeping the top-left where it is.
    //
    // The one rule the window has is asked as on every other path: the size
    // goes through sizeConstraint as a corner resize (ResizeAxis::Both) and
    // is floored at minWidth/minHeight, so a constrained window cannot be
    // put into a shape it would refuse to be dragged into. The size it took
    // may therefore differ from the one asked for; getSize says which. Nothing
    // contains it within the display: an app resizing a window on purpose is
    // told what size to make it.
    //
    // Fires WindowOptions::onResize like a drag does - the window resized,
    // and the content that lays out from that callback wants to hear about
    // it however it happened. No-op on iOS.
    void setSize(Point size);

    // Minimizes to the Dock / taskbar (macOS miniaturize, Windows
    // SW_MINIMIZE). Lets borderless windows with web-rendered window
    // controls offer the standard button. No-op under headless and on iOS.
    void minimize();

    // Maximizes (macOS zoom, Windows SW_MAXIMIZE), or restores the previous
    // frame when already maximized — the standard caption-button toggle.
    // No-op under headless and on iOS.
    void toggleMaximize();

    // Mouse lock for relative-motion input (FPS-style mouse look). While
    // locked the cursor is hidden and pinned in place, and mouse movement
    // keeps streaming to the content view as mouseMoved events whose
    // MouseEvent::delta carries the motion. The lock expresses intent: it
    // engages while the window has key focus, suspends when focus is lost so
    // the cursor works in other apps, and re-engages when focus returns.
    void setMouseLocked(bool locked);
    bool isMouseLocked() const;

    // Keyboard state.
    bool isKeyPressed(uint16_t virtualKeyCode) const;
    bool isShiftPressed() const;
    bool isControlPressed() const;
    bool isAltPressed() const;
    bool isCommandPressed() const;
    ModifierKeys getModifiers() const;

    // Observable window events (e.g. key-focus changes); see WindowEvents.
    WindowEvents events;

private:
    WindowOptions options;

    // The back-pointer View::getWindow() reads, owned as a member so it is
    // cleared by the window's own destruction rather than by a line in each
    // platform's destructor - three of which are `= default`, and a fourth
    // would be written the day a fourth platform arrives. A view outliving its
    // window then reports no window instead of a dangling one.
    struct ContentViewLink
    {
        ~ContentViewLink() { attach(nullptr, nullptr); }

        void attach(View* view, Window* window)
        {
            if (contentView != nullptr)
                contentView->ownerWindow = nullptr;

            contentView = view;

            if (contentView != nullptr)
                contentView->ownerWindow = window;
        }

        View* contentView = nullptr;
    };

    ContentViewLink contentLink;

    struct Native;
    Pimpl<Native> impl;
};

} // namespace eacp::Graphics
