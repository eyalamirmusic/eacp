#pragma once

#include "LinuxInput-Linux.h"
#include "WaylandDisplay-Linux.h"

#include <memory>

// wl_seat, translated into the events View.h already knows about. The protocol
// only: what the seat's events mean is in LinuxInput-Linux.h, and shared with
// every other Linux backend. Every entry point tolerates there being no seat:
// a headless Weston advertises none.

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

    void setSeat(wl_seat* seatToUse);
    void releaseSeat();

    // Polled keyboard state, in the native (evdev) unit.
    bool isKeyPressed(uint32_t evdevCode) const;
    Vector<uint32_t> getPressedCodes() const;
    ModifierKeys getModifiers() const;

    // What the key would type on the current layout with no modifiers applied.
    std::string characterForCode(uint32_t evdevCode) const;

    WaylandWindowSurface* getKeyboardFocus() const { return keyboardWindow; }

    // The serial wl_data_device.set_selection wants: the last keyboard event
    // on a surface of ours, and zero while something else has the focus.
    uint32_t getSelectionSerial() const;

    // Null and {} until a pointer has entered something.
    WaylandWindowSurface* getPointerWindow() const { return pointerWindow; }
    Point getPointerPosition() const { return pointerState.getPosition(); }

    void refreshCursor();

    // Locks the pointer only while the window also has keyboard focus.
    void updateMouseLock(WaylandWindowSurface& window);

    void windowDestroyed(WaylandWindowSurface& window);
    void surfaceDestroyed(wl_surface* surface);

private:
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
    void keyboardEntered(uint32_t serial, wl_surface* surface, wl_array* keys);
    void keyboardLeft();
    void keyChanged(uint32_t serial, uint32_t time, uint32_t code, bool pressed);
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

    friend struct WaylandSeatDispatch;

    WaylandDisplay& display;

    wl_seat* seat = nullptr;
    wl_pointer* pointer = nullptr;
    wl_keyboard* keyboard = nullptr;

    wl_cursor_theme* cursorTheme = nullptr;
    wl_surface* cursorSurface = nullptr;
    CursorTracker cursor;

    // The tracker's position is already in pointerWindow's content points.
    wl_surface* pointerSurface = nullptr;
    WaylandWindowSurface* pointerWindow = nullptr;
    PointerTracker pointerState;
    uint32_t pointerEnterSerial = 0;
    uint32_t lastPointerSerial = 0;
    uint32_t pointerTime = 0;

    // A leave held back until the frame closes; an enter may follow it there.
    WaylandWindowSurface* leavingWindow = nullptr;

    bool pendingMove = false;
    Point pendingMoveDelta;
    Point rawDelta;
    bool hasRawDelta = false;

    // One wl_pointer.frame's worth, dispatched when the frame closes.
    WheelTracker wheel;
    uint32_t wheelTime = 0;

    zwp_locked_pointer_v1* lockedPointer = nullptr;
    zwp_relative_pointer_v1* relativePointer = nullptr;
    WaylandWindowSurface* lockedWindow = nullptr;

    WaylandWindowSurface* keyboardWindow = nullptr;
    uint32_t keyTime = 0;
    uint32_t keyboardSerial = 0;

    XkbKeyboardState keyboardState;
    KeyRepeat repeat;
};
} // namespace eacp::Graphics
