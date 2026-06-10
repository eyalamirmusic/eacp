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

    Vector<WindowFlags> flags;

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
