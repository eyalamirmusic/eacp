#pragma once

#include "../Primitives/Primitives.h"
#include "LinuxWindowSurface-Linux.h"

#include <xcb/xcb.h>
#include <xcb/xcb_cursor.h>

#include <algorithm>
#include <cstdint>
#include <limits>
#include <memory>
#include <optional>

// The process's one connection to the X server. File-scope names carry an
// x11/X11 prefix: this is one unity TU under EACP_CI_BUILD.

namespace eacp::Graphics
{
class View;
class X11Input;

// A position on the wire is a signed 16-bit number and a size an unsigned one,
// while both start as points an app may put anything at all in.
inline int16_t x11ClampPosition(long value)
{
    constexpr auto lowest = (long) std::numeric_limits<int16_t>::min();
    constexpr auto highest = (long) std::numeric_limits<int16_t>::max();

    return (int16_t) std::clamp(value, lowest, highest);
}

inline uint16_t x11ClampSize(long value)
{
    constexpr auto highest = (long) std::numeric_limits<uint16_t>::max();

    return (uint16_t) std::clamp(value, 1L, highest);
}

// A Window on this connection: the neutral half plus the toplevel id every
// piece of X11 glue starts from.
struct X11WindowSurface : LinuxWindowSurface
{
    X11WindowSurface();

    // Zero while the window is headless or the connection failed.
    xcb_window_t getWindow() const { return nativeSurface.window; }

    void setWindow(xcb_window_t window);

    // Every event the connection routed here, a view child's included: only
    // the native knows what one of its own windows means.
    virtual void handleEvent(const xcb_generic_event_t& event);
};

struct X11WindowTarget
{
    xcb_window_t window = 0;
    X11WindowSurface* windowSurface = nullptr;

    // Null for the window's own toplevel, set for a view's child window.
    View* view = nullptr;
};

// What the display and the frame pacer need of the output the windows are on.
struct X11OutputInfo
{
    Rect frame;

    // Millihertz, so 60 Hz is 60000, and zero when the mode is unknown.
    int refreshMilliHz = 0;
};

// Interned in one batch as the connection comes up, so the whole set costs one
// round trip rather than one each.
struct X11Atoms
{
    xcb_atom_t wmProtocols = XCB_ATOM_NONE;
    xcb_atom_t wmDeleteWindow = XCB_ATOM_NONE;
    xcb_atom_t wmState = XCB_ATOM_NONE;
    xcb_atom_t wmChangeState = XCB_ATOM_NONE;
    xcb_atom_t netWmName = XCB_ATOM_NONE;
    xcb_atom_t netWmPid = XCB_ATOM_NONE;
    xcb_atom_t utf8String = XCB_ATOM_NONE;
    xcb_atom_t netWmState = XCB_ATOM_NONE;
    xcb_atom_t netWmStateAbove = XCB_ATOM_NONE;
    xcb_atom_t netWmStateMaximizedHorz = XCB_ATOM_NONE;
    xcb_atom_t netWmStateMaximizedVert = XCB_ATOM_NONE;
    xcb_atom_t netWmStateFullscreen = XCB_ATOM_NONE;
    xcb_atom_t netWmStateHidden = XCB_ATOM_NONE;
    xcb_atom_t netActiveWindow = XCB_ATOM_NONE;
    xcb_atom_t motifWmHints = XCB_ATOM_NONE;
    xcb_atom_t clipboard = XCB_ATOM_NONE;
    xcb_atom_t targets = XCB_ATOM_NONE;
    xcb_atom_t incr = XCB_ATOM_NONE;
    xcb_atom_t netWmWindowType = XCB_ATOM_NONE;
    xcb_atom_t netWmWindowTypeNormal = XCB_ATOM_NONE;
};

class X11Connection
{
public:
    X11Connection();
    ~X11Connection();

    X11Connection(const X11Connection&) = delete;
    X11Connection& operator=(const X11Connection&) = delete;

    bool isValid() const { return connection != nullptr; }

    // False once the server has gone: windows made afterwards come up
    // surfaceless, exactly as headless ones do.
    bool isConnected() const { return connected; }

    xcb_connection_t* getConnection() const { return connection; }
    xcb_screen_t* getScreen() const { return screen; }

    const X11Atoms& getAtoms() const { return atoms; }

    // The core keyboard, and -1 when the server has no XKB extension.
    int32_t getKeyboardDeviceId() const { return keyboardDeviceId; }

    // Null when no cursor theme could be opened.
    xcb_cursor_context_t* getCursorContext() const { return cursors; }

    // The core pointer and keyboard of this connection. Never null once the
    // connection came up.
    X11Input* getInput() const { return input.get(); }

    // The RandR primary output, falling back to the root window's size where
    // there is no RandR or no primary, and nothing at all with no connection.
    // Read once and kept: four round trips is far too much for the frame
    // pacer, which asks on every request. Stage 5's RandR change events are
    // what will have to invalidate it (plan.md D7).
    const std::optional<X11OutputInfo>& getPrimaryOutput() const;

    void registerWindow(const X11WindowTarget& target);
    void unregisterWindow(xcb_window_t window);
    X11WindowTarget findWindow(xcb_window_t window) const;

    void flush();

    // XKB's events name a device rather than a window, so they are routed
    // here instead of to a surface; the input glue installs the handler.
    std::function<void(const xcb_generic_event_t&)> onXkbEvent =
        [](const xcb_generic_event_t&) {};

private:
    void internAtoms();
    void setupXkb();
    void setupRandr();
    void setupXfixes();
    void openLoopSource();
    void closeLoopSource();
    void prepareForPoll();
    void readAndDispatch();
    void dispatch(const xcb_generic_event_t& event);
    void reportError(const xcb_generic_error_t& error);

    // Fired once, when a poll or a flush says the server has gone.
    void checkForConnectionLoss();
    void connectionLost();

    xcb_connection_t* connection = nullptr;
    xcb_screen_t* screen = nullptr;
    xcb_cursor_context_t* cursors = nullptr;

    std::unique_ptr<X11Input> input;

    X11Atoms atoms;

    mutable std::optional<X11OutputInfo> primaryOutput;

    Vector<X11WindowTarget> windows;

    int screenNumber = 0;
    int loopFd = -1;
    int32_t keyboardDeviceId = -1;
    uint8_t xkbEventBase = 0;
    bool randrAvailable = false;
    bool connected = false;
};

// Null when no display could be reached. Deliberately not gated on
// linuxPreferredWindowSystem(): an EmbeddedView is X11 whatever this copy
// prefers, because the id its host handed it is one.
X11Connection* x11Connection();
} // namespace eacp::Graphics
