#pragma once

#include "PointerTrackers.h"

#include <xkbcommon/xkbcommon.h>

#include <memory>
#include <span>

// What a seat's events mean, with none of the protocol that carried them: the
// keymap and the repeat here, the click count, the scroll a frame adds up to and
// the cursor shape in PointerTrackers.h, which the web shares. Wayland and X11
// both feed these; the evdev keycode is the unit throughout, being what
// wl_keyboard.key carries and what an X11 keycode is once 8 has been subtracted.

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
std::span<const char* const> linuxCursorNames(MouseCursor::Shape cursor);

class XkbKeyboardState
{
public:
    XkbKeyboardState();
    ~XkbKeyboardState();

    XkbKeyboardState(const XkbKeyboardState&) = delete;
    XkbKeyboardState& operator=(const XkbKeyboardState&) = delete;

    // For a server that hands its keymap over as an object rather than as
    // text, which is how X11 does it.
    constexpr xkb_context* getContext() const { return context; }

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
    Vector<uint32_t> getPressedCodes() const;

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
} // namespace eacp::Graphics
