#include "LinuxInput-Linux.h"

#include <eacp/Core/Threads/EventLoop.h>
#include <eacp/Core/Threads/Timer.h>

#include <cmath>
#include <linux/input-event-codes.h>

// File-scope names carry a linux prefix: this is one unity TU under
// EACP_CI_BUILD.

namespace eacp::Graphics
{
namespace
{
// No seat event carries a click count, so this is the framework's own figure:
// 400ms is what X11, GTK and Qt all default to.
constexpr uint32_t linuxDoubleClickIntervalMs = 400;
constexpr float linuxDoubleClickSlopPoints = 5.f;

// An evdev keycode is an xkb one once 8 has been added to it.
constexpr uint32_t linuxXkbKeycodeOffset = 8;

std::string linuxUtf8ForKey(xkb_state* state, uint32_t xkbCode)
{
    if (state == nullptr)
        return {};

    auto size = xkb_state_key_get_utf8(state, xkbCode, nullptr, 0);

    if (size <= 0)
        return {};

    auto text = std::string((size_t) size, '\0');
    xkb_state_key_get_utf8(state, xkbCode, text.data(), (size_t) size + 1);

    return text;
}
} // namespace

double linuxTimestamp(uint32_t milliseconds)
{
    return (double) milliseconds / 1000.0;
}

MouseButton linuxButtonFromEvdev(uint32_t code)
{
    switch (code)
    {
        case BTN_LEFT:
            return MouseButton::Left;
        case BTN_RIGHT:
            return MouseButton::Right;
        case BTN_MIDDLE:
            return MouseButton::Middle;
        default:
            return MouseButton::Other;
    }
}

std::span<const char* const> linuxCursorNames(MouseCursor cursor)
{
    static const char* const arrow[] = {"left_ptr", "default", "arrow"};
    static const char* const iBeam[] = {"xterm", "text", "ibeam"};
    static const char* const hand[] = {"hand2", "pointer", "hand1"};
    static const char* const leftRight[] = {
        "sb_h_double_arrow", "ew-resize", "col-resize"};
    static const char* const upDown[] = {
        "sb_v_double_arrow", "ns-resize", "row-resize"};
    static const char* const crosshair[] = {"crosshair", "cross"};

    switch (cursor)
    {
        case MouseCursor::IBeam:
            return iBeam;
        case MouseCursor::PointingHand:
            return hand;
        case MouseCursor::ResizeLeftRight:
            return leftRight;
        case MouseCursor::ResizeUpDown:
            return upDown;
        case MouseCursor::Crosshair:
            return crosshair;
        case MouseCursor::Default:
        default:
            return arrow;
    }
}

XkbKeyboardState::XkbKeyboardState()
    : context(xkb_context_new(XKB_CONTEXT_NO_FLAGS))
{
}

XkbKeyboardState::~XkbKeyboardState()
{
    releaseKeymap();

    if (context != nullptr)
        xkb_context_unref(context);
}

void XkbKeyboardState::releaseKeymap()
{
    if (plainState != nullptr)
        xkb_state_unref(plainState);

    if (state != nullptr)
        xkb_state_unref(state);

    if (keymap != nullptr)
        xkb_keymap_unref(keymap);

    plainState = nullptr;
    state = nullptr;
    keymap = nullptr;
}

bool XkbKeyboardState::setKeymapFromText(const char* text)
{
    if (context == nullptr || text == nullptr)
        return false;

    return setKeymap(xkb_keymap_new_from_string(
        context, text, XKB_KEYMAP_FORMAT_TEXT_V1, XKB_KEYMAP_COMPILE_NO_FLAGS));
}

bool XkbKeyboardState::setKeymap(xkb_keymap* newKeymap)
{
    if (newKeymap == nullptr)
        return false;

    releaseKeymap();

    keymap = newKeymap;
    state = xkb_state_new(keymap);
    plainState = xkb_state_new(keymap);

    return true;
}

void XkbKeyboardState::setModifiers(uint32_t depressed,
                                    uint32_t latched,
                                    uint32_t locked,
                                    uint32_t group)
{
    if (state != nullptr)
        xkb_state_update_mask(state, depressed, latched, locked, 0, 0, group);

    if (plainState != nullptr)
        xkb_state_update_mask(plainState, 0, 0, 0, 0, 0, group);
}

ModifierKeys XkbKeyboardState::getModifiers() const
{
    if (state == nullptr)
        return {};

    auto active = [this](const char* name)
    {
        return xkb_state_mod_name_is_active(state, name, XKB_STATE_MODS_EFFECTIVE)
               > 0;
    };

    // Super/Logo stands in for Command, as the Windows key does on Windows.
    return {active(XKB_MOD_NAME_SHIFT),
            active(XKB_MOD_NAME_CTRL),
            active(XKB_MOD_NAME_ALT),
            active(XKB_MOD_NAME_LOGO)};
}

bool XkbKeyboardState::keyRepeats(uint32_t evdevCode) const
{
    if (keymap == nullptr)
        return false;

    return xkb_keymap_key_repeats(keymap, evdevCode + linuxXkbKeycodeOffset) != 0;
}

std::string XkbKeyboardState::textForKey(uint32_t evdevCode) const
{
    return linuxUtf8ForKey(state, evdevCode + linuxXkbKeycodeOffset);
}

std::string XkbKeyboardState::plainTextForKey(uint32_t evdevCode) const
{
    return linuxUtf8ForKey(plainState, evdevCode + linuxXkbKeycodeOffset);
}

void XkbKeyboardState::setPressed(uint32_t evdevCode, bool pressed)
{
    if (!pressed)
    {
        pressedCodes.removeAllMatches(evdevCode);
        return;
    }

    if (!pressedCodes.contains(evdevCode))
        pressedCodes.add(evdevCode);
}

void XkbKeyboardState::clearPressed()
{
    pressedCodes.clear();
}

bool XkbKeyboardState::isPressed(uint32_t evdevCode) const
{
    return pressedCodes.contains(evdevCode);
}

struct KeyRepeat::Pending
{
    uint32_t code = 0;
    uint64_t generation = 0;
    std::unique_ptr<Threads::Timer> timer;
};

KeyRepeat::KeyRepeat() = default;
KeyRepeat::~KeyRepeat() = default;

void KeyRepeat::setRate(int newRateHz, Time::MS newDelay)
{
    rateHz = newRateHz;
    delay = newDelay;

    if (rateHz <= 0)
        stop();
}

void KeyRepeat::start(uint32_t evdevCode)
{
    stop();

    if (rateHz <= 0)
        return;

    pending = std::make_unique<Pending>();
    pending->code = evdevCode;
    pending->generation = ++generation;

    Threads::callAfter(delay,
                       [this, started = pending->generation, evdevCode]
                       {
                           if (pending == nullptr || pending->generation != started)
                               return;

                           pending->timer = std::make_unique<Threads::Timer>(
                               [this, evdevCode] { onRepeat(evdevCode); }, rateHz);
                       });
}

void KeyRepeat::stop()
{
    // Bumped, so a stale callAfter finds a generation it does not recognise.
    ++generation;
    pending.reset();
}

void KeyRepeat::stopFor(uint32_t evdevCode)
{
    if (pending != nullptr && pending->code == evdevCode)
        stop();
}

int PointerTracker::pressed(MouseButton button, uint32_t timeMilliseconds)
{
    auto near =
        std::abs(position.x - lastClickPosition.x) <= linuxDoubleClickSlopPoints
        && std::abs(position.y - lastClickPosition.y) <= linuxDoubleClickSlopPoints;
    auto soon = timeMilliseconds - lastClickTime <= linuxDoubleClickIntervalMs;

    clickCount = (near && soon && button == lastClickButton) ? clickCount + 1 : 1;
    lastClickTime = timeMilliseconds;
    lastClickButton = button;
    lastClickPosition = position;

    buttonHeld = true;
    heldButton = button;
    downPosition = position;

    return clickCount;
}

void WheelTracker::addDelta(Point delta)
{
    this->delta.x += delta.x;
    this->delta.y += delta.y;

    pending = true;
}

void WheelTracker::addNotches(Point notches)
{
    this->notches.x += notches.x;
    this->notches.y += notches.y;

    hasNotches = true;
    pending = true;
}

void WheelTracker::setSource(bool isPrecise, bool isGesture)
{
    precise = isPrecise;
    gesture = isGesture;
}

void WheelTracker::setStopped()
{
    stopped = true;
    pending = true;
}

Point WheelTracker::getDelta() const
{
    return (!precise && hasNotches) ? notches : delta;
}

void WheelTracker::endFrame()
{
    delta = {};
    notches = {};
    hasNotches = false;
    pending = false;
    stopped = false;
}

bool CursorTracker::setShape(MouseCursor newShape)
{
    if (newShape == shape && !hidden)
        return false;

    shape = newShape;
    hidden = false;

    return true;
}

bool CursorTracker::setHidden(bool shouldHide)
{
    if (hidden == shouldHide)
        return false;

    hidden = shouldHide;

    return true;
}
} // namespace eacp::Graphics
