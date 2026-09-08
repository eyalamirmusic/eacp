#include "WaylandInput-Linux.h"

#include "../Graphics/Keyboard-Linux.h"
#include "LinuxWindowSurface-Linux.h"

#include <eacp/Core/Utils/Environment.h>

#include <wayland-cursor.h>

#include <algorithm>
#include <cstdlib>
#include <sys/mman.h>
#include <unistd.h>

// wl_seat, turned into MouseEvents and KeyEvents. A headless Weston advertises
// no seat, so none of this runs under the compositor tests.

namespace eacp::Graphics
{
namespace
{
// No protocol event carries a cursor size; XCURSOR_SIZE is the convention.
constexpr int waylandDefaultCursorSize = 24;

float waylandFixedToFloat(wl_fixed_t value)
{
    return (float) wl_fixed_to_double(value);
}

bool waylandProxySupports(void* proxy, int since)
{
    if (proxy == nullptr)
        return false;

    return (int) wl_proxy_get_version(static_cast<wl_proxy*>(proxy)) >= since;
}
} // namespace

struct WaylandSeatDispatch
{
    static WaylandInput& self(void* data)
    {
        return *static_cast<WaylandInput*>(data);
    }

    static void seatCapabilities(void* data, wl_seat*, uint32_t capabilities)
    {
        auto& input = self(data);

        if ((capabilities & WL_SEAT_CAPABILITY_POINTER) != 0)
            input.bindPointer();
        else
            input.releasePointer();

        if ((capabilities & WL_SEAT_CAPABILITY_KEYBOARD) != 0)
            input.bindKeyboard();
        else
            input.releaseKeyboard();
    }

    static void seatName(void*, wl_seat*, const char*) {}

    static void pointerEnter(void* data,
                             wl_pointer*,
                             uint32_t serial,
                             wl_surface* surface,
                             wl_fixed_t x,
                             wl_fixed_t y)
    {
        self(data).pointerEntered(serial, surface, x, y);
    }

    static void pointerLeave(void* data, wl_pointer*, uint32_t, wl_surface* surface)
    {
        self(data).pointerLeft(surface);
    }

    static void pointerMotion(
        void* data, wl_pointer*, uint32_t time, wl_fixed_t x, wl_fixed_t y)
    {
        self(data).pointerMoved(time, x, y);
    }

    static void pointerButton(void* data,
                              wl_pointer*,
                              uint32_t serial,
                              uint32_t time,
                              uint32_t button,
                              uint32_t state)
    {
        self(data).pointerButtonChanged(
            serial, time, button, state == WL_POINTER_BUTTON_STATE_PRESSED);
    }

    static void pointerAxis(
        void* data, wl_pointer*, uint32_t time, uint32_t axis, wl_fixed_t value)
    {
        self(data).pointerAxis(time, axis, waylandFixedToFloat(value));
    }

    static void pointerFrame(void* data, wl_pointer*)
    {
        self(data).endPointerFrame();
    }

    static void pointerAxisSource(void* data, wl_pointer*, uint32_t source)
    {
        self(data).pointerAxisSource(source);
    }

    static void pointerAxisStop(void* data, wl_pointer*, uint32_t, uint32_t)
    {
        self(data).pointerAxisStopped();
    }

    static void
        pointerAxisDiscrete(void* data, wl_pointer*, uint32_t axis, int32_t discrete)
    {
        self(data).pointerAxisNotches(axis, (float) discrete);
    }

    static void
        pointerAxisValue120(void* data, wl_pointer*, uint32_t axis, int32_t value120)
    {
        self(data).pointerAxisNotches(axis, (float) value120 / 120.f);
    }

    static void pointerAxisDirection(void*, wl_pointer*, uint32_t, uint32_t) {}

    static void relativeMotion(void* data,
                               zwp_relative_pointer_v1*,
                               uint32_t,
                               uint32_t,
                               wl_fixed_t dx,
                               wl_fixed_t dy,
                               wl_fixed_t unacceleratedX,
                               wl_fixed_t unacceleratedY)
    {
        self(data).pointerMovedRelative(
            {waylandFixedToFloat(dx), waylandFixedToFloat(dy)},
            {waylandFixedToFloat(unacceleratedX),
             waylandFixedToFloat(unacceleratedY)});
    }

    static void keyboardKeymap(
        void* data, wl_keyboard*, uint32_t format, int32_t fd, uint32_t size)
    {
        self(data).keymapArrived(format, fd, size);
    }

    static void keyboardEnter(void* data,
                              wl_keyboard*,
                              uint32_t serial,
                              wl_surface* surface,
                              wl_array* keys)
    {
        self(data).keyboardEntered(serial, surface, keys);
    }

    static void keyboardLeave(void* data, wl_keyboard*, uint32_t, wl_surface*)
    {
        self(data).keyboardLeft();
    }

    static void keyboardKey(void* data,
                            wl_keyboard*,
                            uint32_t serial,
                            uint32_t time,
                            uint32_t key,
                            uint32_t state)
    {
        self(data).keyChanged(
            serial, time, key, state == WL_KEYBOARD_KEY_STATE_PRESSED);
    }

    static void keyboardModifiers(void* data,
                                  wl_keyboard*,
                                  uint32_t,
                                  uint32_t depressed,
                                  uint32_t latched,
                                  uint32_t locked,
                                  uint32_t group)
    {
        self(data).modifiersChanged(depressed, latched, locked, group);
    }

    static void
        keyboardRepeatInfo(void* data, wl_keyboard*, int32_t rate, int32_t delay)
    {
        self(data).repeatInfoChanged(rate, delay);
    }

    static const wl_seat_listener seatListener;
    static const wl_pointer_listener pointerListener;
    static const wl_keyboard_listener keyboardListener;
    static const zwp_relative_pointer_v1_listener relativePointerListener;
};

const wl_seat_listener WaylandSeatDispatch::seatListener {
    .capabilities = WaylandSeatDispatch::seatCapabilities,
    .name = WaylandSeatDispatch::seatName,
};

const wl_pointer_listener WaylandSeatDispatch::pointerListener {
    .enter = WaylandSeatDispatch::pointerEnter,
    .leave = WaylandSeatDispatch::pointerLeave,
    .motion = WaylandSeatDispatch::pointerMotion,
    .button = WaylandSeatDispatch::pointerButton,
    .axis = WaylandSeatDispatch::pointerAxis,
    .frame = WaylandSeatDispatch::pointerFrame,
    .axis_source = WaylandSeatDispatch::pointerAxisSource,
    .axis_stop = WaylandSeatDispatch::pointerAxisStop,
    .axis_discrete = WaylandSeatDispatch::pointerAxisDiscrete,
    .axis_value120 = WaylandSeatDispatch::pointerAxisValue120,
    .axis_relative_direction = WaylandSeatDispatch::pointerAxisDirection,
};

const wl_keyboard_listener WaylandSeatDispatch::keyboardListener {
    .keymap = WaylandSeatDispatch::keyboardKeymap,
    .enter = WaylandSeatDispatch::keyboardEnter,
    .leave = WaylandSeatDispatch::keyboardLeave,
    .key = WaylandSeatDispatch::keyboardKey,
    .modifiers = WaylandSeatDispatch::keyboardModifiers,
    .repeat_info = WaylandSeatDispatch::keyboardRepeatInfo,
};

const zwp_relative_pointer_v1_listener WaylandSeatDispatch::relativePointerListener {
    .relative_motion = WaylandSeatDispatch::relativeMotion,
};

WaylandInput::WaylandInput(WaylandDisplay& displayToUse)
    : display(displayToUse)
{
    repeat.onRepeat = [this](uint32_t code) { deliverKey(code, true, true); };
}

WaylandInput::~WaylandInput()
{
    repeat.stop();
    disengageMouseLock();
    releaseSeat();

    if (cursorSurface != nullptr)
        wl_surface_destroy(cursorSurface);

    if (cursorTheme != nullptr)
        wl_cursor_theme_destroy(cursorTheme);
}

void WaylandInput::setSeat(wl_seat* seatToUse)
{
    releaseSeat();

    seat = seatToUse;

    if (seat != nullptr)
        wl_seat_add_listener(seat, &WaylandSeatDispatch::seatListener, this);
}

void WaylandInput::releaseSeat()
{
    releasePointer();
    releaseKeyboard();

    seat = nullptr;
}

void WaylandInput::bindPointer()
{
    if (pointer != nullptr || seat == nullptr)
        return;

    pointer = wl_seat_get_pointer(seat);
    wl_pointer_add_listener(pointer, &WaylandSeatDispatch::pointerListener, this);

    if (display.getCompositor() != nullptr && cursorSurface == nullptr)
        cursorSurface = wl_compositor_create_surface(display.getCompositor());

    if (cursorTheme == nullptr && display.getShm() != nullptr)
    {
        auto themeName = getEnvValue("XCURSOR_THEME");
        auto sizeText = getEnvValue("XCURSOR_SIZE");
        auto size = sizeText.empty() ? waylandDefaultCursorSize
                                     : std::atoi(sizeText.c_str());

        cursorTheme =
            wl_cursor_theme_load(themeName.empty() ? nullptr : themeName.c_str(),
                                 size > 0 ? size : waylandDefaultCursorSize,
                                 display.getShm());
    }

    // Kept while the pointer exists, not only while locked: rawDelta needs it.
    if (auto* manager = display.getRelativePointers())
    {
        relativePointer =
            zwp_relative_pointer_manager_v1_get_relative_pointer(manager, pointer);
        zwp_relative_pointer_v1_add_listener(
            relativePointer, &WaylandSeatDispatch::relativePointerListener, this);
    }
}

void WaylandInput::releasePointer()
{
    disengageMouseLock();

    if (relativePointer != nullptr)
    {
        zwp_relative_pointer_v1_destroy(relativePointer);
        relativePointer = nullptr;
    }

    if (pointer != nullptr)
    {
        if (waylandProxySupports(pointer, WL_POINTER_RELEASE_SINCE_VERSION))
            wl_pointer_release(pointer);
        else
            wl_pointer_destroy(pointer);

        pointer = nullptr;
    }

    pointerSurface = nullptr;
    pointerWindow = nullptr;
    leavingWindow = nullptr;
}

void WaylandInput::bindKeyboard()
{
    if (keyboard != nullptr || seat == nullptr)
        return;

    keyboard = wl_seat_get_keyboard(seat);
    wl_keyboard_add_listener(keyboard, &WaylandSeatDispatch::keyboardListener, this);
}

void WaylandInput::releaseKeyboard()
{
    repeat.stop();
    setKeyboardFocus(nullptr);
    keyboardState.clearPressed();

    if (keyboard != nullptr)
    {
        if (waylandProxySupports(keyboard, WL_KEYBOARD_RELEASE_SINCE_VERSION))
            wl_keyboard_release(keyboard);
        else
            wl_keyboard_destroy(keyboard);

        keyboard = nullptr;
    }
}

void WaylandInput::pointerEntered(uint32_t serial,
                                  wl_surface* surface,
                                  wl_fixed_t x,
                                  wl_fixed_t y)
{
    auto target = display.findSurface(surface);

    pointerEnterSerial = serial;
    pointerSurface = surface;
    pointerWindow = target.window;

    // An event on a view's subsurface is in that subsurface's own space, so
    // the view's origin is added back on to reach window content points.
    auto local = Point {waylandFixedToFloat(x), waylandFixedToFloat(y)};
    auto origin =
        target.view != nullptr ? linuxViewOriginInWindow(*target.view) : Point {};

    pointerState.setPosition({local.x + origin.x, local.y + origin.y});

    // Crossing from the toplevel onto one of its subsurfaces is not an exit.
    if (leavingWindow == pointerWindow)
        leavingWindow = nullptr;

    cursor.setHidden(false);
    applyCursor();

    pendingMove = true;
    pendingMoveDelta = {};

    if (!waylandProxySupports(pointer, WL_POINTER_FRAME_SINCE_VERSION))
        endPointerFrame();
}

void WaylandInput::pointerLeft(wl_surface* surface)
{
    if (surface != nullptr && surface != pointerSurface)
        return;

    // Held rather than dispatched: the next event may be an enter on another
    // surface of the same window. Resolved at the frame.
    leavingWindow = pointerWindow;
    pointerSurface = nullptr;
    pointerWindow = nullptr;
    pendingMove = false;

    if (!waylandProxySupports(pointer, WL_POINTER_FRAME_SINCE_VERSION))
        endPointerFrame();
}

void WaylandInput::pointerMoved(uint32_t time, wl_fixed_t x, wl_fixed_t y)
{
    auto target = display.findSurface(pointerSurface);
    auto origin =
        target.view != nullptr ? linuxViewOriginInWindow(*target.view) : Point {};

    auto moved =
        Point {waylandFixedToFloat(x) + origin.x, waylandFixedToFloat(y) + origin.y};

    auto previous = pointerState.getPosition();

    pendingMoveDelta = {moved.x - previous.x, moved.y - previous.y};
    pointerState.setPosition(moved);
    pendingMove = true;
    pointerTime = time;

    if (!waylandProxySupports(pointer, WL_POINTER_FRAME_SINCE_VERSION))
        endPointerFrame();
}

// A locked pointer sends no motion events; relative motion is all there is.
void WaylandInput::pointerMovedRelative(Point delta, Point unaccelerated)
{
    rawDelta = unaccelerated;
    hasRawDelta = true;

    if (lockedPointer == nullptr)
        return;

    pendingMoveDelta = delta;
    pendingMove = true;

    if (!waylandProxySupports(pointer, WL_POINTER_FRAME_SINCE_VERSION))
        endPointerFrame();
}

void WaylandInput::pointerButtonChanged(uint32_t serial,
                                        uint32_t time,
                                        uint32_t code,
                                        bool pressed)
{
    lastPointerSerial = serial;
    pointerTime = time;

    if (pointerWindow == nullptr || pointerWindow->contentView == nullptr)
        return;

    auto event = MouseEvent {};
    event.pos = pointerState.getPosition();
    event.button = linuxButtonFromEvdev(code);
    event.modifiers = getModifiers();
    event.timestamp = linuxTimestamp(time);

    if (pressed)
    {
        event.type = MouseEventType::Down;
        event.clickCount = pointerState.pressed(event.button, time);
    }
    else
    {
        pointerState.released();

        event.type = MouseEventType::Up;
        event.clickCount = pointerState.getClickCount();
    }

    dispatchMouse(event);
}

void WaylandInput::pointerAxis(uint32_t time, uint32_t axis, float value)
{
    // Wayland's axis is positive downwards; MouseEvent::delta is not.
    if (axis == WL_POINTER_AXIS_VERTICAL_SCROLL)
        wheel.addDelta({0.f, -value});
    else
        wheel.addDelta({-value, 0.f});

    wheelTime = time;
}

// The notch figure wins for a wheel: MouseEvent promises lines there.
void WaylandInput::pointerAxisNotches(uint32_t axis, float notches)
{
    if (axis == WL_POINTER_AXIS_VERTICAL_SCROLL)
        wheel.addNotches({0.f, -notches});
    else
        wheel.addNotches({-notches, 0.f});
}

void WaylandInput::pointerAxisSource(uint32_t source)
{
    wheel.setSource(source == WL_POINTER_AXIS_SOURCE_FINGER
                        || source == WL_POINTER_AXIS_SOURCE_CONTINUOUS,
                    source == WL_POINTER_AXIS_SOURCE_FINGER);
}

void WaylandInput::pointerAxisStopped()
{
    wheel.setStopped();
}

// In the order a view expects: the exit, then the move, then the wheel.
void WaylandInput::endPointerFrame()
{
    if (leavingWindow != nullptr)
    {
        if (leavingWindow->contentView != nullptr)
        {
            auto event = MouseEvent {};
            event.type = MouseEventType::Exited;
            event.pos = pointerState.getPosition();
            event.modifiers = getModifiers();

            leavingWindow->contentView->dispatchMouseEvent(event);
        }

        leavingWindow = nullptr;
    }

    if (pendingMove && pointerWindow != nullptr)
    {
        auto event = MouseEvent {};
        event.pos = pointerState.getPosition();
        event.delta = pendingMoveDelta;
        event.rawDelta = hasRawDelta ? rawDelta : pendingMoveDelta;
        event.button = pointerState.getHeldButton();
        event.modifiers = getModifiers();
        event.clickCount = pointerState.getClickCount();
        event.timestamp = linuxTimestamp(pointerTime);

        // Only Dragged and Up go to the view that captured the mouse down; a
        // plain Moved is re-hit-tested, and would lose a grab in progress.
        event.type = pointerState.isButtonHeld() ? MouseEventType::Dragged
                                                 : MouseEventType::Moved;

        dispatchMouse(event);
        refreshCursor();
    }

    pendingMove = false;
    pendingMoveDelta = {};
    hasRawDelta = false;
    rawDelta = {};

    dispatchWheel();
}

void WaylandInput::dispatchWheel()
{
    if (!wheel.isPending())
        return;

    auto event = MouseEvent {};
    event.type = MouseEventType::Wheel;
    event.pos = pointerState.getPosition();
    event.downPos = event.pos;
    event.modifiers = getModifiers();
    event.preciseScrolling = wheel.isPrecise();
    event.timestamp = linuxTimestamp(wheelTime);
    event.delta = wheel.getDelta();

    if (wheel.isGesture())
        event.scrollPhase =
            wheel.hasStopped() ? ScrollPhase::Ended : ScrollPhase::Changed;

    wheel.endFrame();

    if (pointerWindow == nullptr || pointerWindow->contentView == nullptr)
        return;

    auto empty = event.delta.x == 0.f && event.delta.y == 0.f;

    if (!empty || event.scrollPhase == ScrollPhase::Ended)
        pointerWindow->contentView->dispatchMouseEvent(event);
}

void WaylandInput::dispatchMouse(MouseEvent event)
{
    if (pointerWindow == nullptr || pointerWindow->contentView == nullptr)
        return;

    event.downPos = event.type == MouseEventType::Wheel
                        ? event.pos
                        : pointerState.getDownPosition();

    pointerWindow->contentView->dispatchMouseEvent(event);
}

void WaylandInput::refreshCursor()
{
    if (pointerWindow == nullptr || pointerWindow->contentView == nullptr)
        return;

    auto* contentView = pointerWindow->contentView;
    auto* hit = contentView->hitTest(pointerState.getPosition());
    auto shape =
        hit != nullptr ? hit->getMouseCursor() : contentView->getMouseCursor();

    if (cursor.setShape(shape))
        applyCursor();
}

void WaylandInput::applyCursor()
{
    if (pointer == nullptr)
        return;

    if (cursor.isHidden())
    {
        wl_pointer_set_cursor(pointer, pointerEnterSerial, nullptr, 0, 0);
        return;
    }

    if (cursorTheme == nullptr || cursorSurface == nullptr)
        return;

    wl_cursor* shape = nullptr;

    for (const auto* name: linuxCursorNames(cursor.getShape()))
    {
        shape = wl_cursor_theme_get_cursor(cursorTheme, name);

        if (shape != nullptr)
            break;
    }

    if (shape == nullptr || shape->image_count == 0)
        return;

    auto* image = shape->images[0];
    auto* buffer = wl_cursor_image_get_buffer(image);

    if (buffer == nullptr)
        return;

    wl_pointer_set_cursor(pointer,
                          pointerEnterSerial,
                          cursorSurface,
                          (int32_t) image->hotspot_x,
                          (int32_t) image->hotspot_y);

    wl_surface_attach(cursorSurface, buffer, 0, 0);
    wl_surface_damage_buffer(
        cursorSurface, 0, 0, (int32_t) image->width, (int32_t) image->height);
    wl_surface_commit(cursorSurface);
}

void WaylandInput::updateMouseLock(WaylandWindowSurface& window)
{
    auto wanted = window.mouseLockIntent && keyboardWindow == &window;

    if (wanted)
        engageMouseLock(window);
    else if (lockedWindow == &window)
        disengageMouseLock();
}

// A compositor without pointer-constraints is not an error.
void WaylandInput::engageMouseLock(WaylandWindowSurface& window)
{
    if (lockedPointer != nullptr || pointer == nullptr
        || window.getSurface() == nullptr)
        return;

    auto* constraints = display.getPointerConstraints();

    if (constraints == nullptr)
        return;

    lockedPointer = zwp_pointer_constraints_v1_lock_pointer(
        constraints,
        window.getSurface(),
        pointer,
        nullptr,
        ZWP_POINTER_CONSTRAINTS_V1_LIFETIME_PERSISTENT);

    lockedWindow = &window;

    cursor.setHidden(true);
    applyCursor();
}

void WaylandInput::disengageMouseLock()
{
    if (lockedPointer != nullptr)
    {
        zwp_locked_pointer_v1_destroy(lockedPointer);
        lockedPointer = nullptr;
    }

    lockedWindow = nullptr;

    if (cursor.setHidden(false))
        applyCursor();
}

void WaylandInput::keymapArrived(uint32_t format, int32_t fd, uint32_t size)
{
    if (format != WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1)
    {
        ::close(fd);
        return;
    }

    auto* text = ::mmap(nullptr, size, PROT_READ, MAP_PRIVATE, fd, 0);

    if (text == MAP_FAILED)
    {
        ::close(fd);
        return;
    }

    keyboardState.setKeymapFromText(static_cast<const char*>(text));

    ::munmap(text, size);
    ::close(fd);
}

void WaylandInput::keyboardEntered(uint32_t serial,
                                   wl_surface* surface,
                                   wl_array* keys)
{
    auto target = display.findSurface(surface);

    keyboardSerial = serial;
    keyboardState.clearPressed();

    if (keys != nullptr)
    {
        const auto* codes = static_cast<const uint32_t*>(keys->data);
        auto count = keys->size / sizeof(uint32_t);

        for (size_t i = 0; i < count; ++i)
            keyboardState.setPressed(codes[i], true);
    }

    setKeyboardFocus(target.window);
}

void WaylandInput::keyboardLeft()
{
    repeat.stop();

    // The matching key-ups go to whoever took focus, so a state kept across
    // the change would report keys stuck down forever.
    keyboardState.clearPressed();

    setKeyboardFocus(nullptr);
}

// The compositor rejects a selection taken with no focus, so the answer with
// none is zero rather than a serial that has gone stale.
uint32_t WaylandInput::getSelectionSerial() const
{
    return keyboardWindow != nullptr ? keyboardSerial : 0;
}

void WaylandInput::setKeyboardFocus(WaylandWindowSurface* window)
{
    if (keyboardWindow == window)
        return;

    auto* previous = keyboardWindow;
    keyboardWindow = window;

    if (previous != nullptr)
    {
        previous->onKeyboardFocus(false);
        updateMouseLock(*previous);
    }

    if (keyboardWindow != nullptr)
    {
        keyboardWindow->onKeyboardFocus(true);
        updateMouseLock(*keyboardWindow);
    }
}

void WaylandInput::keyChanged(uint32_t serial,
                              uint32_t time,
                              uint32_t code,
                              bool pressed)
{
    keyboardSerial = serial;
    keyTime = time;

    keyboardState.setPressed(code, pressed);

    deliverKey(code, pressed, false);

    if (pressed && keyboardState.keyRepeats(code))
        repeat.start(code);
    else if (!pressed)
        repeat.stopFor(code);
}

void WaylandInput::modifiersChanged(uint32_t depressed,
                                    uint32_t latched,
                                    uint32_t locked,
                                    uint32_t group)
{
    keyboardState.setModifiers(depressed, latched, locked, group);
}

void WaylandInput::repeatInfoChanged(int32_t rate, int32_t delay)
{
    repeat.setRate(rate, Time::MS {(int64_t) std::max(delay, 0)});
}

void WaylandInput::deliverKey(uint32_t code, bool down, bool isRepeat)
{
    if (keyboardWindow == nullptr || keyboardWindow->contentView == nullptr)
        return;

    auto event = KeyEvent {};
    event.keyCode = linuxKeyCodeFromEvdev(code);
    event.type = down ? KeyEventType::Down : KeyEventType::Up;
    event.modifiers = getModifiers();
    event.isRepeat = isRepeat;
    event.timestamp = linuxTimestamp(keyTime);
    event.characters = keyboardState.textForKey(code);
    event.charactersIgnoringModifiers = keyboardState.plainTextForKey(code);

    if (down)
        keyboardWindow->contentView->keyDown(event);
    else
        keyboardWindow->contentView->keyUp(event);
}

bool WaylandInput::isKeyPressed(uint32_t evdevCode) const
{
    return keyboardState.isPressed(evdevCode);
}

Vector<uint32_t> WaylandInput::getPressedCodes() const
{
    return keyboardState.getPressedCodes();
}

ModifierKeys WaylandInput::getModifiers() const
{
    return keyboardState.getModifiers();
}

std::string WaylandInput::characterForCode(uint32_t evdevCode) const
{
    return keyboardState.plainTextForKey(evdevCode);
}

void WaylandInput::windowDestroyed(WaylandWindowSurface& window)
{
    if (lockedWindow == &window)
        disengageMouseLock();

    if (keyboardWindow == &window)
    {
        repeat.stop();
        keyboardState.clearPressed();
        keyboardWindow = nullptr;
    }

    if (pointerWindow == &window)
    {
        pointerWindow = nullptr;
        pointerSurface = nullptr;
        pendingMove = false;
    }

    if (leavingWindow == &window)
        leavingWindow = nullptr;
}

void WaylandInput::surfaceDestroyed(wl_surface* surface)
{
    if (surface != nullptr && surface == pointerSurface)
    {
        pointerSurface = nullptr;
        pointerWindow = nullptr;
        pendingMove = false;
    }
}
} // namespace eacp::Graphics
