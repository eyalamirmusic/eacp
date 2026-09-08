#pragma once

#include "LinuxInput-Linux.h"
#include "LinuxSeat-Linux.h"
#include "X11Connection-Linux.h"

// The core pointer and keyboard of one X11 connection, turned into the events
// View.h already knows about. The protocol only: what a device's events mean
// is in LinuxInput-Linux.h, and shared with the Wayland backend.
//
// A window's handleEvent hands the raw events straight here, because the
// window has nothing to add: unlike Wayland, where every surface is its own
// coordinate space, an X11 event that reached the toplevel is already in the
// toplevel's points, whether it was selected there or propagated up from a
// view's child window.

namespace eacp::Graphics
{
class X11Input final : public LinuxSeat
{
public:
    explicit X11Input(X11Connection& connectionToUse);
    ~X11Input() override;

    X11Input(const X11Input&) = delete;
    X11Input& operator=(const X11Input&) = delete;

    bool isKeyPressed(uint32_t evdevCode) const override;
    Vector<uint32_t> getPressedCodes() const override;
    ModifierKeys getModifiers() const override;
    std::string characterForCode(uint32_t evdevCode) const override;

    X11WindowSurface* getKeyboardFocus() const override { return keyboardWindow; }
    X11WindowSurface* getPointerWindow() const override { return pointerWindow; }
    Point getPointerPosition() const override { return pointerState.getPosition(); }

    void refreshCursor() override;

    // Everything a window's handleEvent forwards. Each takes the window the
    // event was selected on, because an X11 event names an id and not an
    // object.
    void keyChanged(X11WindowSurface& window,
                    const xcb_key_press_event_t& event,
                    bool pressed);
    void focusChanged(X11WindowSurface& window,
                      const xcb_focus_in_event_t& event,
                      bool focused);
    void pointerEntered(X11WindowSurface& window,
                        const xcb_enter_notify_event_t& event);
    void pointerLeft(X11WindowSurface& window,
                     const xcb_leave_notify_event_t& event);
    void pointerMoved(X11WindowSurface& window,
                      const xcb_motion_notify_event_t& event);
    void buttonChanged(X11WindowSurface& window,
                       const xcb_button_press_event_t& event,
                       bool pressed);

    // Grabs the pointer only while the window also has keyboard focus.
    void updateMouseLock(X11WindowSurface& window);

    void windowDestroyed(X11WindowSurface& window);

private:
    void selectXkbEvents();
    void loadKeymap();
    void readServerState();
    void requestDetectableAutoRepeat();
    void handleXkbEvent(const xcb_generic_event_t& event);

    void setKeyboardFocus(X11WindowSurface* window);
    void deliverKey(uint32_t evdevCode, bool down, bool repeat);

    void setPointerWindow(X11WindowSurface* window);
    void dispatchMouse(MouseEvent event);
    void dispatchWheel(uint32_t time);
    void wheelFromButton(uint8_t button);

    void applyCursor();
    xcb_cursor_t cursorForShape(MouseCursor shape);

    void takeFocusOnClick(X11WindowSurface& window, xcb_timestamp_t time);

    void engageMouseLock(X11WindowSurface& window);
    void disengageMouseLock();
    Point lockCentre() const;
    void warpToLockCentre();

    xcb_connection_t* xcb() const { return connection.getConnection(); }

    struct CachedCursor
    {
        MouseCursor shape = MouseCursor::Default;
        xcb_cursor_t cursor = XCB_CURSOR_NONE;
    };

    X11Connection& connection;

    int32_t deviceId = -1;

    // The server does the repeating on X11, so KeyRepeat is not used here: a
    // press of a key already down is one of the server's repeats.
    bool detectableAutoRepeat = false;

    XkbKeyboardState keyboardState;

    X11WindowSurface* keyboardWindow = nullptr;
    xcb_timestamp_t keyTime = 0;

    X11WindowSurface* pointerWindow = nullptr;
    PointerTracker pointerState;

    CursorTracker cursor;
    Vector<CachedCursor> cursorCache;
    bool cursorHidden = false;

    WheelTracker wheel;

    X11WindowSurface* lockedWindow = nullptr;
};
} // namespace eacp::Graphics
