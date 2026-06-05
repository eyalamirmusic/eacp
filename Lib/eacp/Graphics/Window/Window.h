#pragma once

#include <functional>
#include <optional>
#include <string>
#include <ea_data_structures/Structures/Vector.h>
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

// Observable window events. Assign a handler to react; exposed as the public
// `events` member on Window. All handlers fire on the main thread.
struct WindowEvents
{
    // Called when the window gains (true) or loses (false) key focus. Lets the
    // app track main-window foreground state — e.g. to reveal a companion
    // overlay when the user switches to another app. macOS only; other platforms
    // never invoke it (yet).
    std::function<void(bool isKey)> onActivationChanged;
};

struct WindowOptions
{
    WindowOptions()
    {
        flags.emplace_back(WindowFlags::Titled);
        flags.emplace_back(WindowFlags::Closable);
        flags.emplace_back(WindowFlags::Miniaturizable);
        flags.emplace_back(WindowFlags::Resizable);
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

    // When true, the title bar draws no background of its own, so a
    // FullSizeContentView's content shows through beneath the traffic
    // lights. Without this, macOS (notably Sequoia) paints the titlebar's
    // translucent material over the content as a grey band.
    bool titlebarTransparent = false;

    // The hairline separator drawn under the title bar (macOS Big Sur+).
    // Set false (NSTitlebarSeparatorStyleNone) to drop it so a custom header
    // blends seamlessly into the content with no chrome line.
    bool showTitlebarSeparator = true;

    // macOS: inset of the window controls (close / minimise / zoom) from the
    // window's top-left, in points — mirrors Electron's trafficLightPosition.
    // Only meaningful with a hidden/transparent title bar (FullSizeContentView).
    // Unset leaves the buttons at their default macOS position. Re-applied on
    // resize so the buttons don't drift back after fullscreen transitions.
    std::optional<Point> trafficLightPosition;

    // macOS: background colour shown behind the content view — before the web
    // view first paints and during live resize. Unset uses the system window
    // background; set to black to avoid a white flash on launch/resize.
    std::optional<Color> backgroundColor;

    // Minimum content size in points (0 = no minimum). Content-relative, to
    // match width/height and the resize callbacks above.
    int minWidth = 0;
    int minHeight = 0;

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
    // other applications (macOS makeKeyAndOrderFront + activateIgnoringOtherApps,
    // Windows ShowWindow + SetForegroundWindow). No-op under headless and on iOS.
    void toFront();

    // Keyboard state (tracked from window events)
    bool isKeyPressed(uint16_t virtualKeyCode) const;
    bool isShiftPressed() const;
    bool isControlPressed() const;
    bool isAltPressed() const;
    bool isCommandPressed() const;
    ModifierKeys getModifiers() const;

    // Observable window events (e.g. key-focus changes). Attach EA::Listeners.
    WindowEvents events;

private:
    WindowOptions options;

    struct Native;
    Pimpl<Native> impl;
};

} // namespace eacp::Graphics