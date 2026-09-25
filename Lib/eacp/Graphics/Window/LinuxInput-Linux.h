#pragma once

#include "../View/View.h"

#include <xkbcommon/xkbcommon.h>

#include <memory>
#include <span>

// What a seat's events mean, with none of the protocol that carried them: the
// keymap, the repeat, the click count, the scroll a frame adds up to and the
// cursor shape. Wayland and X11 both feed these; the evdev keycode is the unit
// throughout, being what wl_keyboard.key carries and what an X11 keycode is
// once 8 has been subtracted.

namespace eacp::Threads
{
class Timer;
}

namespace eacp::Graphics
{
// Seconds since an arbitrary origin, as MouseEvent::timestamp wants them; only
// differences are meaningful.
double linuxTimestamp(uint32_t milliseconds);

MouseButton linuxButtonFromEvdev(uint32_t code);

// Most likely name first; a theme with none of them falls back to the arrow.
std::span<const char* const> linuxCursorNames(MouseCursor cursor);

class XkbKeyboardState
{
public:
    XkbKeyboardState();
    ~XkbKeyboardState();

    XkbKeyboardState(const XkbKeyboardState&) = delete;
    XkbKeyboardState& operator=(const XkbKeyboardState&) = delete;

    // For a server that hands its keymap over as an object rather than as
    // text, which is how X11 does it.
    xkb_context* getContext() const { return context; }

    // Both take the keymap on: the previous one and the states made from it
    // are dropped. False leaves the last keymap in place.
    bool setKeymapFromText(const char* text);
    bool setKeymap(xkb_keymap* newKeymap);

    // The server's modifier state. The plain state follows the layout group
    // and nothing else, so it says what a key types with nothing held.
    void setModifiers(uint32_t depressed,
                      uint32_t latched,
                      uint32_t locked,
                      uint32_t group);

    ModifierKeys getModifiers() const;

    bool keyRepeats(uint32_t evdevCode) const;

    std::string textForKey(uint32_t evdevCode) const;
    std::string plainTextForKey(uint32_t evdevCode) const;

    void setPressed(uint32_t evdevCode, bool pressed);
    void clearPressed();

    bool isPressed(uint32_t evdevCode) const;
    Vector<uint32_t> getPressedCodes() const { return pressedCodes; }

private:
    void releaseKeymap();

    xkb_context* context = nullptr;
    xkb_keymap* keymap = nullptr;
    xkb_state* state = nullptr;
    xkb_state* plainState = nullptr;

    Vector<uint32_t> pressedCodes;
};

// A repeat is a delay and then a rate; Timer has one interval, so the delay is
// a callAfter. A generation counter stops a stale one starting a repeat.
class KeyRepeat
{
public:
    KeyRepeat();
    ~KeyRepeat();

    KeyRepeat(const KeyRepeat&) = delete;
    KeyRepeat& operator=(const KeyRepeat&) = delete;

    // A rate of zero or less is a server that wants no repeats at all.
    void setRate(int rateHz, Time::MS delay);

    void start(uint32_t evdevCode);
    void stop();

    // The key that was let go is only the repeating one some of the time.
    void stopFor(uint32_t evdevCode);

    std::function<void(uint32_t)> onRepeat = [](uint32_t) {};

private:
    struct Pending;

    int rateHz = 0;
    Time::MS delay {0};
    uint64_t generation = 0;
    std::unique_ptr<Pending> pending;
};

// What no window system reports: the click count and the slop it allows, the
// button held down and where it went down.
class PointerTracker
{
public:
    Point getPosition() const { return position; }
    void setPosition(Point newPosition) { position = newPosition; }

    // The click count this press earns.
    int pressed(MouseButton button, uint32_t timeMilliseconds);
    void released() { buttonHeld = false; }

    bool isButtonHeld() const { return buttonHeld; }
    MouseButton getHeldButton() const { return heldButton; }
    Point getDownPosition() const { return downPosition; }
    int getClickCount() const { return clickCount; }

private:
    Point position;
    Point downPosition;

    bool buttonHeld = false;
    MouseButton heldButton = MouseButton::Left;

    int clickCount = 0;
    uint32_t lastClickTime = 0;
    MouseButton lastClickButton = MouseButton::Left;
    Point lastClickPosition;
};

// One frame's worth of scroll, however the server spelled it out.
class WheelTracker
{
public:
    // Both in MouseEvent::delta's orientation, not the server's.
    void addDelta(Point delta);
    void addNotches(Point notches);

    void setSource(bool isPrecise, bool isGesture);
    void setStopped();

    bool isPending() const { return pending; }
    bool isPrecise() const { return precise; }
    bool isGesture() const { return gesture; }
    bool hasStopped() const { return stopped; }

    // Lines for a notched wheel, points for a trackpad.
    Point getDelta() const;

    // Keeps the source, which is a property of the device and not of a frame.
    void endFrame();

private:
    Point delta;
    Point notches;
    bool hasNotches = false;
    bool pending = false;
    bool precise = false;
    bool gesture = false;
    bool stopped = false;
};

// The shape under the pointer, and whether a mouse lock has hidden it. Both
// setters are true when the backend must apply the cursor again.
class CursorTracker
{
public:
    bool setShape(MouseCursor newShape);
    bool setHidden(bool shouldHide);

    MouseCursor getShape() const { return shape; }
    bool isHidden() const { return hidden; }

private:
    MouseCursor shape = MouseCursor::Default;
    bool hidden = false;
};
} // namespace eacp::Graphics
