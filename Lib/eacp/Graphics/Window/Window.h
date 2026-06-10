#pragma once

#include <functional>
#include <optional>
#include <string>
#include <eacp/Core/Utils/Containers.h>
#include "../Primitives/Primitives.h"
#include "../View/View.h"
#include <eacp/Core/App/App.h>

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
    // Fires when the window gains (true) or loses (false) key focus. macOS
    // only; other platforms never invoke it (yet).
    std::function<void(bool isKey)> onActivationChanged;
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

    // When the user closes the window. If left empty, falls back to
    // Apps::quit when isPrimary is true, or a no-op otherwise.
    Callback onQuit {};

    // Set to false for secondary/popup windows so closing them doesn't
    // terminate the app when onQuit is unset.
    bool isPrimary = true;

    // Called after the window has been resized. Sizes are in points and refer
    // to the content view, not the outer frame.
    ResizeCallback onResize {};

    // Called while the user is dragging the resize corner. Receives the
    // proposed content-view size in points; may be mutated to clamp or
    // snap to constraints.
    WillResizeCallback onWillResize {};

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

    // Minimum content size in points (0 = no minimum). Content-relative, to
    // match width/height and the resize callbacks above.
    int minWidth = 0;
    int minHeight = 0;

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

    // Initial position of the window's top-left corner in screen points,
    // measured from the primary display's top-left (Electron convention).
    // Unset centers the window (macOS) / uses the system default (Windows).
    std::optional<Point> initialPosition;

    // Rounds the window's corners (points). Borderless windows are square
    // by default; set this to get the standard macOS rounded shape. On
    // macOS this makes the window non-opaque with a clear background
    // (overriding backgroundColor) so the clipped content view defines the
    // visible shape. Windows 11 rounds via DWM at the system radius — the
    // value is ignored there; earlier Windows stays square.
    std::optional<float> cornerRadius;

    EA::Vector<WindowFlags> flags;

    Callback effectiveOnQuit() const
    {
        if (onQuit)
            return onQuit;
        return isPrimary ? Callback {Apps::quit} : Callback {[] {}};
    }
};

struct ModifierKeys;

class Window
{
public:
    Window(const WindowOptions& optionsToUse = {});
    ~Window();

    void setTitle(const std::string& title);
    void* getHandle();
    void* getContentViewHandle();

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

    struct Native;
    Pimpl<Native> impl;
};

} // namespace eacp::Graphics
