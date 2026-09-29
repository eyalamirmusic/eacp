#pragma once

#include "../Primitives/Primitives.h"
#include "LinuxWindowSurface-Linux.h"

#include <eacp/Core/Utils/Time.h>

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
class X11Clipboard;
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

    // Set when the server destroyed this window rather than we did - a host
    // taking its own window down, or a KillClient. Every child window inside
    // it went with it, so a view surface still holding one unregisters it and
    // leaves it alone: asking for it to be destroyed again is one BadWindow
    // per presenting view.
    bool inferiorsGone = false;

    // A popup takes no keyboard: the window it pops over stays focused, or a
    // host's editor greys out under the menu. So the click that would
    // otherwise hand this window the keyboard does not.
    bool refusesFocus = false;

    // Every event the connection routed here, a view child's included: only
    // the native knows what one of its own windows means.
    virtual void handleEvent(const xcb_generic_event_t& event);

    // The display's scale changed under an open window. A toplevel takes it,
    // because Xft.dpi is the only thing that names one; an embedded surface
    // does not, because its host already did (EmbeddedView::setPixelsPerPoint).
    virtual void scaleChanged(float newScale);
};

struct X11WindowTarget
{
    xcb_window_t window = 0;
    X11WindowSurface* windowSurface = nullptr;

    // Null for the window's own toplevel, set for a view's child window.
    View* view = nullptr;

    bool operator==(const X11WindowTarget&) const = default;
};

// Everything a window of ours is ever told about, structure and seat alike. A
// view's child window selects Exposure and nothing else, so the pointer and
// key events over one propagate up to the window it is in, already in that
// window's points.
inline constexpr uint32_t x11WindowEventMask =
    XCB_EVENT_MASK_STRUCTURE_NOTIFY | XCB_EVENT_MASK_EXPOSURE
    | XCB_EVENT_MASK_FOCUS_CHANGE | XCB_EVENT_MASK_PROPERTY_CHANGE
    | XCB_EVENT_MASK_KEY_PRESS | XCB_EVENT_MASK_KEY_RELEASE
    | XCB_EVENT_MASK_BUTTON_PRESS | XCB_EVENT_MASK_BUTTON_RELEASE
    | XCB_EVENT_MASK_POINTER_MOTION | XCB_EVENT_MASK_ENTER_WINDOW
    | XCB_EVENT_MASK_LEAVE_WINDOW;

// The pointer half of it, which XI2 takes over whole where the server has it.
// A window that selects an XI2 event and the core event it replaces is a
// window told about the same press twice on some servers and once on others,
// so the core bits come off rather than being relied on to be suppressed.
inline constexpr uint32_t x11PointerEventMask =
    XCB_EVENT_MASK_BUTTON_PRESS | XCB_EVENT_MASK_BUTTON_RELEASE
    | XCB_EVENT_MASK_POINTER_MOTION | XCB_EVENT_MASK_ENTER_WINDOW
    | XCB_EVENT_MASK_LEAVE_WINDOW;

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
    xcb_atom_t text = XCB_ATOM_NONE;
    xcb_atom_t textPlain = XCB_ATOM_NONE;
    xcb_atom_t textPlainUtf8 = XCB_ATOM_NONE;
    xcb_atom_t textUriList = XCB_ATOM_NONE;

    xcb_atom_t netWmWindowType = XCB_ATOM_NONE;
    xcb_atom_t netWmWindowTypeNormal = XCB_ATOM_NONE;
    xcb_atom_t netWmWindowTypePopupMenu = XCB_ATOM_NONE;

    // The property a conversion of ours is delivered into, on the clipboard's
    // own window: named after this framework so no other client's transfer can
    // land on it.
    xcb_atom_t eacpSelection = XCB_ATOM_NONE;
};

// A popup window for as long as it is up. The seat asks it about every press
// before turning one into a MouseEvent, because a press outside a menu belongs
// to the menu: it closes it and goes no further, rather than reaching what it
// landed on - which may be a host's own window, and no window of ours to
// decide for.
struct X11PopupGrab
{
    virtual ~X11PopupGrab() = default;

    // In root pixels, which is the one coordinate space every event carries
    // beside its own window's.
    virtual bool containsRootPoint(Point rootPosition) const = 0;

    // False for a popup the app takes down itself, which keeps the press.
    virtual bool dismissesOnOutsideClick() const = 0;

    // Deferred a turn by the popup: the handler is allowed to destroy the
    // Window it belongs to.
    virtual void requestDismissal() = 0;
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

    // XInput 2.1 or better, which is where a pointer's scroll arrives as a
    // valuator rather than as buttons 4-7 and where a locked pointer can be
    // measured from the device itself. False on an older server and under
    // EACP_X11_NO_XI2=1, and the core pointer events are then the whole of the
    // seat, exactly as they were before any of this.
    bool isXinputAvailable() const { return xinputOpcode != 0; }

    // What a window of ours selects: the mask above, less the pointer bits
    // XI2 is carrying where it is there.
    uint32_t getWindowEventMask() const;

    // The pointer events of one window of ours, for every master device. Both
    // window natives call it; nothing else does.
    void selectPointerEvents(xcb_window_t window);

    // The seat's own selection, on the root and not on a window: the device
    // table and the raw motion a mouse lock reads. The device spec is part of
    // what is being selected rather than a detail of it - a hierarchy notice
    // is only ever sent for all devices at once, and raw motion only for the
    // masters - and each spec keeps a mask of its own on the window.
    void selectXinputEvents(xcb_window_t window, uint16_t device, uint32_t mask);

    // The core pointer and keyboard of this connection. Never null once the
    // connection came up.
    X11Input* getInput() const { return input.get(); }

    // The CLIPBOARD selection of this connection. Never null once the
    // connection came up, and answering the no-clipboard answers once it has
    // gone.
    X11Clipboard* getClipboard() const { return clipboard.get(); }

    // The RandR primary output, falling back to the root window's size where
    // there is no RandR or no primary, and nothing at all with no connection.
    // Read once and kept: four round trips is far too much for the frame
    // pacer, which asks on every request. A RandR screen or output change
    // drops it, so the next question is answered from the server again.
    const std::optional<X11OutputInfo>& getPrimaryOutput() const;

    // Pixels per point for a standalone toplevel: Xft.dpi over 96, and 1
    // wherever the resource database names no dpi. Kept as a fraction rather
    // than rounded to a whole factor - Qt's rule, not GTK's - because a
    // desktop at 150% says 144 and means 1.5, and every pixel the backend
    // derives from it is rounded at the point it is derived.
    float getScale() const { return scale; }

    void registerWindow(const X11WindowTarget& target);
    void unregisterWindow(xcb_window_t window);
    X11WindowTarget findWindow(xcb_window_t window) const;

    // One popup at a time: a menu opened over another takes the pointer from
    // it, and only the popup that took it may give it back.
    void setActivePopup(X11PopupGrab& popup);
    void clearActivePopup(const X11PopupGrab& popup);

    // Null while no menu is up, which is nearly always.
    X11PopupGrab* getActivePopup() const { return activePopup; }

    // A window somebody else owns that one of ours has to hear about: an
    // EmbeddedView's host parent, whose ConfigureNotify its child follows.
    // Apart from the map above because the id is not ours to claim - it may
    // already be a window of this copy's, and registering it there would take
    // that window's own events away from it.
    void watchForeignWindow(const X11WindowTarget& watcher);
    void unwatchForeignWindow(const X11WindowSurface& surface);

    void flush();

    // Dispatches this connection's events, exactly as the loop source would,
    // until `satisfied` answers true or the deadline passes; true when it was
    // satisfied. A synchronous clipboard read is the only caller: the answer
    // it waits for is an ordinary event, and everything that arrives beside it
    // has to reach the window it belongs to rather than be thrown away.
    bool dispatchUntil(const std::function<bool()>& satisfied,
                       Time::Deadline deadline);

    // XKB's events name a device rather than a window, so they are routed
    // here instead of to a surface; the input glue installs the handler.
    std::function<void(const xcb_generic_event_t&)> onXkbEvent =
        [](const xcb_generic_event_t&) {};

private:
    void internAtoms();
    void setupXkb();
    void setupXinput();
    void setupRandr();
    void setupXfixes();
    void setupRoot();
    void openCursorContext();

    // The root's RESOURCE_MANAGER changed: the scale every toplevel is laid
    // out at, and the theme and size every cursor is loaded from, both live in
    // it.
    void resourcesChanged();
    void scaleChanged(float newScale);
    void cursorThemeChanged();

    // A RandR screen or output change: the output's frame and the refresh rate
    // the pacer runs at are read again. Not the scale, which is the resource
    // database's to name and not an output's.
    void outputChanged();
    void openLoopSource();
    void closeLoopSource();
    void prepareForPoll();
    void drainQueuedEvents();
    void readAndDispatch();
    void dispatch(const xcb_generic_event_t& event);
    bool dispatchXinput(const xcb_generic_event_t& event);
    void dispatchToWatchers(const xcb_generic_event_t& event, xcb_window_t window);
    void reportError(const xcb_generic_error_t& error);

    // Fired once, when a poll or a flush says the server has gone.
    void checkForConnectionLoss();
    void connectionLost();

    xcb_connection_t* connection = nullptr;
    xcb_screen_t* screen = nullptr;
    xcb_cursor_context_t* cursors = nullptr;

    std::unique_ptr<X11Input> input;
    std::unique_ptr<X11Clipboard> clipboard;

    X11Atoms atoms;

    mutable std::optional<X11OutputInfo> primaryOutput;

    Vector<X11WindowTarget> windows;
    Vector<X11WindowTarget> watchers;

    X11PopupGrab* activePopup = nullptr;

    float scale = 1.f;

    int screenNumber = 0;
    int loopFd = -1;
    int32_t keyboardDeviceId = -1;
    uint8_t xkbEventBase = 0;
    uint8_t randrEventBase = 0;
    uint8_t xinputOpcode = 0;
    bool randrAvailable = false;
    bool connected = false;
};

// Null when no display could be reached. Deliberately not gated on
// linuxPreferredWindowSystem(): an EmbeddedView is X11 whatever this copy
// prefers, because the id its host handed it is one.
X11Connection* x11Connection();
} // namespace eacp::Graphics
