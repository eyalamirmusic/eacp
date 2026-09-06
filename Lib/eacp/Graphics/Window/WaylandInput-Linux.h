#pragma once

#include "WaylandDisplay-Linux.h"

#include "../View/View.h"

#include <xkbcommon/xkbcommon.h>

#include <memory>

// wl_seat, translated into the events View.h already knows about.
//
// The shape is CompositionHostWindow-Windows.cpp's, because the routing
// decision is the same one: there is a surface per window, none per view, and
// the portable hit-tester in View.cpp decides who gets what. So everything here
// ends at contentView->dispatchMouseEvent / keyDown / keyUp with a position in
// window content points, and no view is ever addressed directly.
//
// Two things are Wayland's own. Pointer events name the surface they are over,
// which may be one of the subsurfaces a presenting view was given rather than
// the toplevel, so a position has to be lifted back into window coordinates
// before it is dispatched. And there is no polled keyboard at all - no
// GetAsyncKeyState, no CGEventSource - so what Keyboard.h queries is a pressed
// set this listener maintains and an xkb state it feeds.
//
// Every entry point tolerates there being no seat. A headless Weston
// advertises none at all, so on CI this object is constructed, never told
// about a seat, and answers every query as an unfocused window would.

struct wl_cursor_theme;

namespace eacp::Graphics
{
class WaylandInput
{
public:
    explicit WaylandInput(WaylandDisplay& displayToUse);
    ~WaylandInput();

    WaylandInput(const WaylandInput&) = delete;
    WaylandInput& operator=(const WaylandInput&) = delete;

    // Binds the seat's devices. Called when the wl_seat global is announced;
    // never called at all on a compositor that advertises no seat.
    void setSeat(wl_seat* seatToUse);
    void releaseSeat();

    // Polled keyboard state, in the native (evdev) unit. Keyboard-Linux.cpp
    // converts to and from framework KeyCodes around these.
    bool isKeyPressed(uint32_t evdevCode) const;
    Vector<uint32_t> getPressedCodes() const;
    ModifierKeys getModifiers() const;

    // What the key would type on the current layout with no modifiers applied,
    // which is what Keyboard::keyCodeToCharacter promises.
    std::string characterForCode(uint32_t evdevCode) const;

    // The window the compositor has given keyboard focus, or null.
    WaylandWindowSurface* getKeyboardFocus() const { return keyboardWindow; }

    // Where the pointer is, in the content points of the window it is over, and
    // which window that is. Null and {} when no pointer has entered anything -
    // which includes every machine with no pointing device.
    WaylandWindowSurface* getPointerWindow() const { return pointerWindow; }
    Point getPointerPosition() const { return pointerPosition; }

    // Re-reads the cursor of the view under the pointer and asks the compositor
    // for it. Called on every pointer motion, and by View::setMouseCursor so a
    // shape set from inside a mouseMoved handler takes effect on that same move.
    void refreshCursor();

    // Applies Window::setMouseLocked's intent: a locked pointer while the
    // window also has keyboard focus, and nothing at all when the compositor
    // offers no pointer-constraints extension.
    void updateMouseLock(WaylandWindowSurface& window);

    // Drops every reference to a window that is going away, so an event
    // arriving after the destructor cannot reach it.
    void windowDestroyed(WaylandWindowSurface& window);

    // The same for one of a window's view surfaces.
    void surfaceDestroyed(wl_surface* surface);

private:
    struct Repeat;

    void bindPointer();
    void bindKeyboard();
    void releasePointer();
    void releaseKeyboard();

    void pointerEntered(uint32_t serial,
                        wl_surface* surface,
                        wl_fixed_t x,
                        wl_fixed_t y);
    void pointerLeft(wl_surface* surface);
    void pointerMoved(uint32_t time, wl_fixed_t x, wl_fixed_t y);
    void pointerMovedRelative(Point delta, Point unaccelerated);
    void pointerButtonChanged(uint32_t serial,
                              uint32_t time,
                              uint32_t code,
                              bool pressed);
    void pointerAxis(uint32_t time, uint32_t axis, float value);
    void pointerAxisNotches(uint32_t axis, float notches);
    void pointerAxisSource(uint32_t source);
    void pointerAxisStopped();
    void endPointerFrame();

    void keymapArrived(uint32_t format, int32_t fd, uint32_t size);
    void keyboardEntered(wl_surface* surface, wl_array* keys);
    void keyboardLeft();
    void keyChanged(uint32_t time, uint32_t code, bool pressed);
    void modifiersChanged(uint32_t depressed,
                          uint32_t latched,
                          uint32_t locked,
                          uint32_t group);
    void repeatInfoChanged(int32_t rate, int32_t delay);

    void engageMouseLock(WaylandWindowSurface& window);
    void disengageMouseLock();

    void dispatchMouse(MouseEvent event);
    void dispatchWheel();
    void applyCursor();

    void setKeyboardFocus(WaylandWindowSurface* window);
    void deliverKey(uint32_t evdevCode, bool down, bool repeat);
    void startRepeat(uint32_t evdevCode);
    void stopRepeat();

    // Wayland's listeners are C function pointers with a void* payload, so the
    // dispatch tables live in the .cpp and reach the object through this.
    friend struct WaylandSeatDispatch;

    WaylandDisplay& display;

    wl_seat* seat = nullptr;
    wl_pointer* pointer = nullptr;
    wl_keyboard* keyboard = nullptr;

    wl_cursor_theme* cursorTheme = nullptr;
    wl_surface* cursorSurface = nullptr;
    MouseCursor cursorShape = MouseCursor::Default;
    bool cursorHidden = false;

    // Pointer focus. `pointerSurface` is the wl_surface the compositor is
    // addressing, which may be a view's subsurface; `pointerWindow` is the
    // window that surface belongs to, and `pointerPosition` is already lifted
    // into that window's content points.
    wl_surface* pointerSurface = nullptr;
    WaylandWindowSurface* pointerWindow = nullptr;
    Point pointerPosition;
    Point pointerDownPosition;
    uint32_t pointerEnterSerial = 0;
    uint32_t lastPointerSerial = 0;
    uint32_t pointerTime = 0;

    // A leave held back until the frame closes, because the enter that follows
    // it in the same frame may be another surface of the same window - which is
    // what crossing onto a presenting view's subsurface looks like on the wire,
    // and is not an exit.
    WaylandWindowSurface* leavingWindow = nullptr;

    bool pendingMove = false;
    Point pendingMoveDelta;
    Point rawDelta;
    bool hasRawDelta = false;

    bool buttonHeld = false;
    MouseButton heldButton = MouseButton::Left;

    // Click counting, which no Wayland event carries: the compositor reports
    // presses and nothing else, so a double click is two presses of the same
    // button close enough in time and space to have been meant as one gesture.
    int clickCount = 0;
    uint32_t lastClickTime = 0;
    MouseButton lastClickButton = MouseButton::Left;
    Point lastClickPosition;

    // One wl_pointer.frame's worth of axis motion, accumulated across the
    // axis / axis_source / axis_value120 events that describe it and dispatched
    // as one MouseEvent when the frame closes.
    Point wheelDelta;
    Point wheelNotches;
    bool hasWheelNotches = false;
    bool wheelPending = false;
    bool wheelPrecise = false;
    bool wheelIsGesture = false;
    bool wheelStopped = false;
    uint32_t wheelTime = 0;

    zwp_locked_pointer_v1* lockedPointer = nullptr;
    zwp_relative_pointer_v1* relativePointer = nullptr;
    WaylandWindowSurface* lockedWindow = nullptr;

    WaylandWindowSurface* keyboardWindow = nullptr;
    uint32_t keyTime = 0;

    xkb_context* xkbContext = nullptr;
    xkb_keymap* keymap = nullptr;

    // Two states over the same keymap: the live one, which carries the
    // modifiers the compositor reports, and a bare one that never gets them.
    // KeyEvent promises both spellings of what a key typed, and the bare state
    // is where charactersIgnoringModifiers comes from.
    xkb_state* xkbState = nullptr;
    xkb_state* xkbPlainState = nullptr;

    Vector<uint32_t> pressedCodes;

    int repeatRateHz = 0;
    Time::MS repeatDelay {0};
    uint64_t repeatGeneration = 0;
    std::unique_ptr<Repeat> repeatState;
};
} // namespace eacp::Graphics
