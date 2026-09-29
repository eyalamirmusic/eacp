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
//
// Where the server has XInput 2.1 the pointer half of that arrives as XI2
// GenericEvents instead, routed straight here by the connection rather than
// through a window, and the core handlers below are what a server without it
// falls back to. The two never run together: a window selecting XI2 pointer
// events leaves the core pointer bits unselected.

namespace eacp::Graphics
{
// One scroll axis of one device. X11 does not report what a frame scrolled: it
// reports a valuator that counts on for as long as the device exists, so the
// scroll is the difference from the last value seen and the first value after
// a device change is a baseline worth nothing.
struct X11ScrollAxis
{
    bool present = false;
    uint16_t number = 0;
    double increment = 0.0;
    double last = 0.0;
    bool hasLast = false;

    // Clicks, in the valuator's own direction - positive is down or right -
    // and a fraction of one from a device that scrolls smoothly.
    float step(double value);
};

struct X11ScrollDevice
{
    uint16_t deviceId = 0;
    X11ScrollAxis vertical;
    X11ScrollAxis horizontal;
};

// A wheel a server turned back into buttons 4-7 for clients that know nothing
// of valuators. The valuator was counted already, so this is the same notch a
// second time; an emulated button that is not a wheel is a touch pressing, and
// that one is wanted.
bool x11IsEmulatedWheelButton(uint32_t flags, uint32_t button);

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

    // Every XI2 GenericEvent on this connection: the pointer events of our own
    // windows, the raw motion a mouse lock measures itself by, and the two
    // notices that say the device table has changed under it.
    void xinputEvent(const xcb_generic_event_t& event);

    // The resource database named a new theme or size and the connection has
    // built a context from it: every cursor loaded out of the old one goes,
    // and the shape the pointer is wearing is set again from the new one.
    void cursorThemeChanged();

    // Everything a window's handleEvent forwards. Each takes the window the
    // event was selected on, because an X11 event names an id and not an
    // object. The pointer four are the fallback for a server with no XI2, and
    // the path a locked pointer's grab still reports through while the pointer
    // is outside the window it is confined to.
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

    // A popup took the pointer, or gave it back: whatever button was held was
    // held in another window, and the up that would have ended it is going to
    // the menu now.
    void popupGrabChanged();

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
    void wheelFromButton(uint32_t button);

    // What the core and the XI2 paths both end in, once the protocol has been
    // turned into a point in the window and a button number.
    void pointerEntering(X11WindowSurface& window,
                         uint8_t mode,
                         uint8_t detail,
                         Point position,
                         uint32_t time);
    void pointerLeaving(X11WindowSurface& window,
                        uint8_t mode,
                        uint8_t detail,
                        Point position,
                        uint32_t time);
    void pointerMovedTo(X11WindowSurface& window, Point position, uint32_t time);
    void buttonAction(X11WindowSurface& window,
                      uint32_t button,
                      bool pressed,
                      Point position,
                      uint32_t time);
    void dispatchMotion(Point delta, Point unaccelerated, uint32_t time);

    // Both halves of a popup's dismissal, asked before a protocol event
    // becomes a MouseEvent or a KeyEvent: a press outside an open menu closes
    // it and is swallowed, and so is Escape.
    bool popupSwallowsButton(uint32_t button, bool pressed, Point rootPosition);
    bool popupSwallowsKey(uint32_t evdevCode, bool pressed);

    // The XI2 half, one function per event the seat selects.
    void setupXinput();
    void readScrollDevices();
    X11ScrollDevice* scrollDeviceFor(uint16_t sourceId);
    void selectRawMotion(bool wanted);

    void xinputCrossing(const xcb_generic_event_t& event, bool entering);
    void xinputButton(const xcb_generic_event_t& event, bool pressed);
    void xinputMotion(const xcb_generic_event_t& event);
    void rawMotion(const xcb_generic_event_t& event);

    // True when the event carried any scroll valuator at all, whether or not
    // it had moved: a wheel is a motion event that moved nothing else.
    bool accumulateScroll(const xcb_generic_event_t& event);

    X11WindowSurface* windowForXinputEvent(const xcb_generic_event_t& event);

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

    Vector<X11ScrollDevice> scrollDevices;

    // A locked pointer's deltas come from XI_RawMotion, which is the device's
    // own report and reaches us whoever holds the pointer grabbed. Both
    // figures a raw event carries are used, and they are the two a Wayland
    // relative pointer gives: the accelerated one is MouseEvent::delta, which
    // is what a pointer moving a slider wants, and the unaccelerated one is
    // rawDelta, which is what aiming a camera wants. The warp that recentres
    // the pointer generates neither, which is the whole reason for this: it
    // generates ordinary motion, and telling that apart from the hand's was
    // never possible from a position alone.
    bool rawMotionActive = false;

    bool xinput = false;
};
} // namespace eacp::Graphics
