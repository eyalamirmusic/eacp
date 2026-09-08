#pragma once

#include "../Graphics/Keyboard.h"
#include "LinuxWindowSurface-Linux.h"
#include "Window.h"

#include <memory>
#include <optional>
#include <string>

// The toplevel behind a Window, whatever window system is under it, and the
// half of one that no window system decides.

namespace eacp::Graphics
{
// Names a .desktop file on Wayland and the WM_CLASS of an X11 toplevel;
// neither carries a per-window icon.
inline constexpr const char* linuxDefaultAppId = "eacp";

inline constexpr Color linuxDefaultWindowBackground = Color::gray(0.f);

// The option-derived state of a toplevel and the behaviour built on it: what
// is left over in a native is protocol. A native holds one and hands it the
// LinuxWindowSurface it is.
struct LinuxWindowState
{
    LinuxWindowState(LinuxWindowSurface& surfaceToUse,
                     const WindowOptions& options,
                     WindowEvents& eventsToUse);

    // onWillResize first, then the aspect ratio and the minimum size.
    void applyConstraints(int& width, int& height) const;

    // Resizes the content view and reports it; silent when the size asked for
    // is the one already there.
    void resizeTo(Point newSize);

    void setActive(bool nowActive);

    // A programmatic move is still a move, so it reports itself.
    void setPosition(Point newPosition);

    // hostWindowVisibilityChanged down the whole content tree.
    void notifyHostVisibility(bool visible);

    // The user asked for the window to close: it hides and says so, or the app
    // quits.
    void closeRequested();

    LinuxWindowSurface& surface;

    // Takes the window off screen without destroying it; set by the native,
    // and only closeRequested calls it.
    Callback unmap = [] {};

    std::string title;
    Callback quitCallback;
    ResizeCallback onResize;
    WillResizeCallback onWillResize;
    WindowEvents* events;

    int minWidth = 0;
    int minHeight = 0;
    std::optional<Point> aspectRatio;
    bool hidesOnClose = false;
    bool resizable = true;
    bool closable = true;
    bool miniaturizable = true;
    bool transparent = false;
    Color background;

    Point position;
    bool active = false;
    bool maximized = false;
};

// What a Window forwards to. Not a LinuxWindowSurface itself: a native is this
// *and* its window system's surface, and one base holding the other would make
// that a diamond.
class LinuxWindowNative
{
public:
    virtual ~LinuxWindowNative() = default;

    virtual LinuxWindowState& getState() = 0;

    // The window system's own object, and null while there is none: the
    // wl_surface on Wayland.
    virtual void* getHandle() = 0;

    virtual void setVisible(bool visible) = 0;
    virtual void setTitle(const std::string& title) = 0;
    virtual void minimize() = 0;
    virtual void toggleMaximize() = 0;
    virtual void setMouseLocked(bool locked) = 0;

    // The native unit is the evdev keycode on every Linux backend.
    virtual bool isKeyPressed(uint16_t nativeKeyCode) = 0;
    virtual ModifierKeys getModifiers() = 0;

    // The content view fills the window, finds it, and — once there is
    // something to show it on — brings it up.
    virtual void setContentView(View* view);

    // Wayland has no global coordinates, so the value put in is the one handed
    // back; a backend that can really move a window overrides this.
    virtual void setPosition(Point newPosition);

    LinuxWindowSurface& getWindowSurface() { return getState().surface; }

    bool isVisible() { return getWindowSurface().mapped; }
    Point getPosition() { return getState().position; }
    bool isMouseLocked() { return getWindowSurface().mouseLockIntent; }
    void* getContentViewHandle();
};

// A window with no window system under it: everything is remembered and
// nothing is shown. What EACP_HEADLESS=1, an unreachable compositor and a
// backend that is not implemented yet all end up with.
std::unique_ptr<LinuxWindowNative>
    makeHeadlessWindowNative(const WindowOptions& options, WindowEvents& events);
} // namespace eacp::Graphics
