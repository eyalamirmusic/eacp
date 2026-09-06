#include "WaylandInput-Linux.h"

#include "../Graphics/Keyboard-Linux.h"
#include "../View/WaylandViewSurface-Linux.h"

#include <eacp/Core/Threads/Timer.h>
#include <eacp/Core/Utils/Environment.h>

#include <wayland-cursor.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <linux/input-event-codes.h>
#include <span>
#include <sys/mman.h>
#include <unistd.h>

// wl_seat, turned into MouseEvents and KeyEvents.
//
// Nothing in here runs on a headless Weston, which advertises no seat at all -
// no pointer, no keyboard, not even an empty one - so the compositor test next
// door cannot exercise a line of it. What is testable without a seat is
// testable without Wayland: the evdev-to-KeyCode table has a suite of its own
// (Tests/Graphics/KeyCodeTests-Linux.cpp), and the routing this file performs
// is the portable hit-tester in View.cpp, which ScrollWheelTests and
// ViewWindowTests already cover from the other side. The part with no coverage
// is the translation between them, and it is written to be as thin as the
// Windows original for exactly that reason.

namespace eacp::Graphics
{
namespace
{
// Two presses of the same button inside this, and close enough together on
// screen, are one gesture. No Wayland event carries a click count - the
// compositor reports presses and nothing more - so this is the framework's own
// figure, and 400ms is what X11, GTK and Qt all default to.
constexpr uint32_t waylandDoubleClickIntervalMs = 400;
constexpr float waylandDoubleClickSlopPoints = 5.f;

// wl_cursor themes are sized in pixels and there is no protocol event for the
// size, only the XCURSOR_SIZE convention every toolkit reads.
constexpr int waylandDefaultCursorSize = 24;

float waylandFixedToFloat(wl_fixed_t value)
{
    return (float) wl_fixed_to_double(value);
}

double waylandTimestamp(uint32_t milliseconds)
{
    // Seconds since an arbitrary origin, which is what NSEvent.timestamp is
    // too. Only differences are meaningful in either.
    return (double) milliseconds / 1000.0;
}

MouseButton waylandButtonFromEvdev(uint32_t code)
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

// The XCursor names for the six shapes MouseCursor names, most-specific first:
// a theme that lacks the modern name usually has the legacy one, and one that
// has neither falls back to the arrow rather than leaving whatever the last
// window set.
std::span<const char* const> waylandCursorNames(MouseCursor cursor)
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

std::string waylandUtf8ForKey(xkb_state* state, uint32_t xkbCode)
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

bool waylandProxySupports(void* proxy, int since)
{
    if (proxy == nullptr)
        return false;

    return (int) wl_proxy_get_version(static_cast<wl_proxy*>(proxy)) >= since;
}
} // namespace

// The repeat a compositor asks for in wl_keyboard.repeat_info: a delay before
// the first repeat, then a rate. Threads::Timer has one fixed interval, so the
// delay is a callAfter and the rate is a Timer built when it fires; the
// generation counter is what stops a stale callAfter from starting a repeat for
// a key that has since come up.
struct WaylandInput::Repeat
{
    uint32_t code = 0;
    uint64_t generation = 0;
    std::unique_ptr<Threads::Timer> timer;
};

// Every C callback the seat needs, in one struct so the unity build sees one
// file-scope name instead of two dozen.
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

    static void keyboardEnter(
        void* data, wl_keyboard*, uint32_t, wl_surface* surface, wl_array* keys)
    {
        self(data).keyboardEntered(surface, keys);
    }

    static void keyboardLeave(void* data, wl_keyboard*, uint32_t, wl_surface*)
    {
        self(data).keyboardLeft();
    }

    static void keyboardKey(void* data,
                            wl_keyboard*,
                            uint32_t,
                            uint32_t time,
                            uint32_t key,
                            uint32_t state)
    {
        self(data).keyChanged(time, key, state == WL_KEYBOARD_KEY_STATE_PRESSED);
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
    xkbContext = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
}

WaylandInput::~WaylandInput()
{
    stopRepeat();
    disengageMouseLock();
    releaseSeat();

    if (xkbPlainState != nullptr)
        xkb_state_unref(xkbPlainState);

    if (xkbState != nullptr)
        xkb_state_unref(xkbState);

    if (keymap != nullptr)
        xkb_keymap_unref(keymap);

    if (xkbContext != nullptr)
        xkb_context_unref(xkbContext);

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

    // Created here rather than at lock time, and kept for as long as the
    // pointer exists: relative motion is the unaccelerated figure
    // MouseEvent::rawDelta promises, and a camera wants it whether or not the
    // pointer happens to be locked. Windows gets the same thing from Raw Input,
    // which it also registers once and leaves registered.
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
    stopRepeat();
    setKeyboardFocus(nullptr);
    pressedCodes.clear();

    if (keyboard != nullptr)
    {
        if (waylandProxySupports(keyboard, WL_KEYBOARD_RELEASE_SINCE_VERSION))
            wl_keyboard_release(keyboard);
        else
            wl_keyboard_destroy(keyboard);

        keyboard = nullptr;
    }
}

// --- pointer -----------------------------------------------------------------

void WaylandInput::pointerEntered(uint32_t serial,
                                  wl_surface* surface,
                                  wl_fixed_t x,
                                  wl_fixed_t y)
{
    auto target = display.findSurface(surface);

    pointerEnterSerial = serial;
    pointerSurface = surface;
    pointerWindow = target.window;

    // A pointer entering one of a presenting view's subsurfaces reports
    // coordinates in that subsurface's own space, so the view's origin inside
    // the window is added back on. Everything downstream then works in window
    // content points, exactly as the Windows client area does.
    auto local = Point {waylandFixedToFloat(x), waylandFixedToFloat(y)};
    auto origin =
        target.view != nullptr ? waylandViewOriginInWindow(*target.view) : Point {};

    pointerPosition = {local.x + origin.x, local.y + origin.y};

    // The same window it was just told it was leaving: one gesture crossing
    // from the toplevel onto a subsurface, not an exit.
    if (leavingWindow == pointerWindow)
        leavingWindow = nullptr;

    cursorHidden = false;
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

    // Held rather than dispatched, because the very next event may be an enter
    // on another surface of the same window - which is what crossing onto a
    // GPUView's subsurface looks like on the wire. Resolved at the frame.
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
        target.view != nullptr ? waylandViewOriginInWindow(*target.view) : Point {};

    auto moved =
        Point {waylandFixedToFloat(x) + origin.x, waylandFixedToFloat(y) + origin.y};

    pendingMoveDelta = {moved.x - pointerPosition.x, moved.y - pointerPosition.y};
    pointerPosition = moved;
    pendingMove = true;
    pointerTime = time;

    if (!waylandProxySupports(pointer, WL_POINTER_FRAME_SINCE_VERSION))
        endPointerFrame();
}

// The locked case. A locked pointer sends no motion at all - it does not move -
// so relative motion is the only movement there is, and the position stays put
// while the deltas keep coming. Unlocked, the accelerated figure is redundant
// with the motion event and only the unaccelerated one is kept.
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

    auto button = waylandButtonFromEvdev(code);

    auto event = MouseEvent {};
    event.pos = pointerPosition;
    event.button = button;
    event.modifiers = getModifiers();
    event.timestamp = waylandTimestamp(time);

    if (pressed)
    {
        auto near = std::abs(pointerPosition.x - lastClickPosition.x)
                        <= waylandDoubleClickSlopPoints
                    && std::abs(pointerPosition.y - lastClickPosition.y)
                           <= waylandDoubleClickSlopPoints;
        auto soon = time - lastClickTime <= waylandDoubleClickIntervalMs;

        clickCount =
            (near && soon && button == lastClickButton) ? clickCount + 1 : 1;
        lastClickTime = time;
        lastClickButton = button;
        lastClickPosition = pointerPosition;

        buttonHeld = true;
        heldButton = button;
        pointerDownPosition = pointerPosition;

        event.type = MouseEventType::Down;
        event.clickCount = clickCount;
    }
    else
    {
        buttonHeld = false;
        event.type = MouseEventType::Up;
        event.clickCount = clickCount;
    }

    dispatchMouse(event);
}

void WaylandInput::pointerAxis(uint32_t time, uint32_t axis, float value)
{
    // Wayland measures the axis in the direction the content scrolls away:
    // positive is a scroll downwards. MouseEvent::delta is the other way round
    // - positive y moves the content down, toward the start of the document -
    // so the sign is flipped once, here.
    if (axis == WL_POINTER_AXIS_VERTICAL_SCROLL)
        wheelDelta.y -= value;
    else
        wheelDelta.x -= value;

    wheelPending = true;
    wheelTime = time;
}

// axis_discrete and axis_value120 both describe the same motion the axis event
// already reported, in notches rather than in surface units. The notch figure
// wins for a wheel, because MouseEvent promises lines for a non-precise device.
void WaylandInput::pointerAxisNotches(uint32_t axis, float notches)
{
    if (axis == WL_POINTER_AXIS_VERTICAL_SCROLL)
        wheelNotches.y -= notches;
    else
        wheelNotches.x -= notches;

    hasWheelNotches = true;
    wheelPending = true;
}

void WaylandInput::pointerAxisSource(uint32_t source)
{
    wheelPrecise = source == WL_POINTER_AXIS_SOURCE_FINGER
                   || source == WL_POINTER_AXIS_SOURCE_CONTINUOUS;
    wheelIsGesture = source == WL_POINTER_AXIS_SOURCE_FINGER;
}

void WaylandInput::pointerAxisStopped()
{
    wheelStopped = true;
    wheelPending = true;
}

// One wl_pointer.frame's worth of everything, dispatched in the order a view
// expects to see it: the exit first, then the move the enter or motion
// implied, then the wheel.
void WaylandInput::endPointerFrame()
{
    if (leavingWindow != nullptr)
    {
        if (leavingWindow->contentView != nullptr)
        {
            auto event = MouseEvent {};
            event.type = MouseEventType::Exited;
            event.pos = pointerPosition;
            event.modifiers = getModifiers();

            leavingWindow->contentView->dispatchMouseEvent(event);
        }

        leavingWindow = nullptr;
    }

    if (pendingMove && pointerWindow != nullptr)
    {
        auto event = MouseEvent {};
        event.pos = pointerPosition;
        event.delta = pendingMoveDelta;
        event.rawDelta = hasRawDelta ? rawDelta : pendingMoveDelta;
        event.button = heldButton;
        event.modifiers = getModifiers();
        event.clickCount = clickCount;
        event.timestamp = waylandTimestamp(pointerTime);

        // A move with a button held is a drag. dispatchMouseEvent only forwards
        // Dragged and Up to the view that captured the mouse down; a plain
        // Moved is re-hit-tested, so without this a title-bar grab is lost the
        // moment the cursor moves - the same trap the Windows file documents.
        event.type = buttonHeld ? MouseEventType::Dragged : MouseEventType::Moved;

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
    if (!wheelPending)
    {
        wheelStopped = false;
        return;
    }

    wheelPending = false;

    if (pointerWindow != nullptr && pointerWindow->contentView != nullptr)
    {
        auto event = MouseEvent {};
        event.type = MouseEventType::Wheel;
        event.pos = pointerPosition;
        event.downPos = pointerPosition;
        event.modifiers = getModifiers();
        event.preciseScrolling = wheelPrecise;
        event.timestamp = waylandTimestamp(wheelTime);

        // Lines for a notched wheel, points for a trackpad - the two units
        // MouseEvent::preciseScrolling exists to tell apart.
        event.delta = (!wheelPrecise && hasWheelNotches) ? wheelNotches : wheelDelta;

        if (wheelIsGesture)
            event.scrollPhase =
                wheelStopped ? ScrollPhase::Ended : ScrollPhase::Changed;

        auto empty = event.delta.x == 0.f && event.delta.y == 0.f;

        if (!empty || event.scrollPhase == ScrollPhase::Ended)
            pointerWindow->contentView->dispatchMouseEvent(event);
    }

    wheelDelta = {};
    wheelNotches = {};
    hasWheelNotches = false;
    wheelStopped = false;
}

void WaylandInput::dispatchMouse(MouseEvent event)
{
    if (pointerWindow == nullptr || pointerWindow->contentView == nullptr)
        return;

    // Where the drag began, in the same window content points as everything
    // else. Wayland surfaces have no screen position, so unlike Windows there
    // is nothing to convert through - a window moving under a stationary
    // pointer changes neither figure.
    event.downPos =
        event.type == MouseEventType::Wheel ? event.pos : pointerDownPosition;

    pointerWindow->contentView->dispatchMouseEvent(event);
}

// --- cursor ------------------------------------------------------------------

void WaylandInput::refreshCursor()
{
    if (pointerWindow == nullptr || pointerWindow->contentView == nullptr)
        return;

    auto* contentView = pointerWindow->contentView;
    auto* hit = contentView->hitTest(pointerPosition);
    auto shape =
        hit != nullptr ? hit->getMouseCursor() : contentView->getMouseCursor();

    if (shape == cursorShape && !cursorHidden)
        return;

    cursorShape = shape;
    cursorHidden = false;
    applyCursor();
}

void WaylandInput::applyCursor()
{
    if (pointer == nullptr)
        return;

    if (cursorHidden)
    {
        wl_pointer_set_cursor(pointer, pointerEnterSerial, nullptr, 0, 0);
        return;
    }

    if (cursorTheme == nullptr || cursorSurface == nullptr)
        return;

    wl_cursor* cursor = nullptr;

    for (const auto* name: waylandCursorNames(cursorShape))
    {
        cursor = wl_cursor_theme_get_cursor(cursorTheme, name);

        if (cursor != nullptr)
            break;
    }

    if (cursor == nullptr || cursor->image_count == 0)
        return;

    auto* image = cursor->images[0];
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

// --- mouse lock ---------------------------------------------------------------

void WaylandInput::updateMouseLock(WaylandWindowSurface& window)
{
    auto wanted = window.mouseLockIntent && keyboardWindow == &window;

    if (wanted)
        engageMouseLock(window);
    else if (lockedWindow == &window)
        disengageMouseLock();
}

// The lock expresses intent (Window::setMouseLocked), so a compositor with
// neither extension is not an error: rawDelta still arrives from the relative
// pointer if that one is there, and the cursor simply stays visible and free.
void WaylandInput::engageMouseLock(WaylandWindowSurface& window)
{
    if (lockedPointer != nullptr || pointer == nullptr || window.surface == nullptr)
        return;

    auto* constraints = display.getPointerConstraints();

    if (constraints == nullptr)
        return;

    lockedPointer = zwp_pointer_constraints_v1_lock_pointer(
        constraints,
        window.surface,
        pointer,
        nullptr,
        ZWP_POINTER_CONSTRAINTS_V1_LIFETIME_PERSISTENT);

    lockedWindow = &window;

    cursorHidden = true;
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

    if (cursorHidden)
    {
        cursorHidden = false;
        applyCursor();
    }
}

// --- keyboard -----------------------------------------------------------------

void WaylandInput::keymapArrived(uint32_t format, int32_t fd, uint32_t size)
{
    if (format != WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1 || xkbContext == nullptr)
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

    auto* newKeymap = xkb_keymap_new_from_string(xkbContext,
                                                 static_cast<const char*>(text),
                                                 XKB_KEYMAP_FORMAT_TEXT_V1,
                                                 XKB_KEYMAP_COMPILE_NO_FLAGS);

    ::munmap(text, size);
    ::close(fd);

    if (newKeymap == nullptr)
        return;

    if (xkbPlainState != nullptr)
        xkb_state_unref(xkbPlainState);

    if (xkbState != nullptr)
        xkb_state_unref(xkbState);

    if (keymap != nullptr)
        xkb_keymap_unref(keymap);

    keymap = newKeymap;
    xkbState = xkb_state_new(keymap);
    xkbPlainState = xkb_state_new(keymap);
}

void WaylandInput::keyboardEntered(wl_surface* surface, wl_array* keys)
{
    auto target = display.findSurface(surface);

    pressedCodes.clear();

    if (keys != nullptr)
    {
        const auto* codes = static_cast<const uint32_t*>(keys->data);
        auto count = keys->size / sizeof(uint32_t);

        for (size_t i = 0; i < count; ++i)
            pressedCodes.add(codes[i]);
    }

    setKeyboardFocus(target.window);
}

void WaylandInput::keyboardLeft()
{
    stopRepeat();

    // The matching key-ups go to whoever took focus, so a tracked state kept
    // across the change would report keys stuck down forever - the same reason
    // the Windows backend resets on WM_KILLFOCUS.
    pressedCodes.clear();

    setKeyboardFocus(nullptr);
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

void WaylandInput::keyChanged(uint32_t time, uint32_t code, bool pressed)
{
    keyTime = time;

    if (pressed)
    {
        if (!pressedCodes.contains(code))
            pressedCodes.add(code);
    }
    else
    {
        pressedCodes.removeAllMatches(code);
    }

    deliverKey(code, pressed, false);

    if (pressed && keymap != nullptr
        && xkb_keymap_key_repeats(keymap, code + 8) != 0)
        startRepeat(code);
    else if (!pressed && repeatState != nullptr && repeatState->code == code)
        stopRepeat();
}

void WaylandInput::modifiersChanged(uint32_t depressed,
                                    uint32_t latched,
                                    uint32_t locked,
                                    uint32_t group)
{
    if (xkbState != nullptr)
        xkb_state_update_mask(xkbState, depressed, latched, locked, 0, 0, group);

    // The bare state follows the layout group and nothing else, so
    // charactersIgnoringModifiers is what the key types on this layout with
    // neither shift nor a dead key applied.
    if (xkbPlainState != nullptr)
        xkb_state_update_mask(xkbPlainState, 0, 0, 0, 0, 0, group);
}

void WaylandInput::repeatInfoChanged(int32_t rate, int32_t delay)
{
    repeatRateHz = rate;
    repeatDelay = Time::MS {(int64_t) std::max(delay, 0)};

    if (rate <= 0)
        stopRepeat();
}

void WaylandInput::deliverKey(uint32_t code, bool down, bool repeat)
{
    if (keyboardWindow == nullptr || keyboardWindow->contentView == nullptr)
        return;

    auto event = KeyEvent {};
    event.keyCode = waylandKeyCodeFromEvdev(code);
    event.type = down ? KeyEventType::Down : KeyEventType::Up;
    event.modifiers = getModifiers();
    event.isRepeat = repeat;
    event.timestamp = waylandTimestamp(keyTime);
    event.characters = waylandUtf8ForKey(xkbState, code + 8);
    event.charactersIgnoringModifiers = waylandUtf8ForKey(xkbPlainState, code + 8);

    if (down)
        keyboardWindow->contentView->keyDown(event);
    else
        keyboardWindow->contentView->keyUp(event);
}

void WaylandInput::startRepeat(uint32_t code)
{
    stopRepeat();

    if (repeatRateHz <= 0)
        return;

    repeatState = std::make_unique<Repeat>();
    repeatState->code = code;
    repeatState->generation = ++repeatGeneration;

    auto generation = repeatState->generation;

    Threads::callAfter(
        repeatDelay,
        [this, generation, code]
        {
            if (repeatState == nullptr || repeatState->generation != generation)
                return;

            repeatState->timer = std::make_unique<Threads::Timer>(
                [this, code] { deliverKey(code, true, true); }, repeatRateHz);
        });
}

void WaylandInput::stopRepeat()
{
    // Bumped rather than only cleared, so a callAfter already scheduled for the
    // key that has just come up finds a generation it does not recognise.
    ++repeatGeneration;
    repeatState.reset();
}

// --- polled state --------------------------------------------------------------

bool WaylandInput::isKeyPressed(uint32_t evdevCode) const
{
    return pressedCodes.contains(evdevCode);
}

Vector<uint32_t> WaylandInput::getPressedCodes() const
{
    return pressedCodes;
}

ModifierKeys WaylandInput::getModifiers() const
{
    if (xkbState == nullptr)
        return {};

    auto active = [this](const char* name)
    {
        return xkb_state_mod_name_is_active(xkbState, name, XKB_STATE_MODS_EFFECTIVE)
               > 0;
    };

    // The Super/Logo key stands in for Command, as it does everywhere a
    // platform has no Command of its own - the Windows backend maps it to the
    // Windows key for the same reason.
    return {active(XKB_MOD_NAME_SHIFT),
            active(XKB_MOD_NAME_CTRL),
            active(XKB_MOD_NAME_ALT),
            active(XKB_MOD_NAME_LOGO)};
}

std::string WaylandInput::characterForCode(uint32_t evdevCode) const
{
    return waylandUtf8ForKey(xkbPlainState, evdevCode + 8);
}

void WaylandInput::windowDestroyed(WaylandWindowSurface& window)
{
    if (lockedWindow == &window)
        disengageMouseLock();

    if (keyboardWindow == &window)
    {
        stopRepeat();
        pressedCodes.clear();
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
