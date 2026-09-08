#include "X11Input-Linux.h"

#include "../Graphics/Keyboard-Linux.h"

#include <xcb/xfixes.h>
#include <xkbcommon/xkbcommon-x11.h>

// xcb/xkb.h names a struct field `explicit`, which C++ will not take.
#define explicit explicit_
#include <xcb/xkb.h>
#undef explicit

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <linux/input-event-codes.h>

// File-scope names carry an x11/X11 prefix: this is one unity TU under
// EACP_CI_BUILD.

namespace eacp::Graphics
{
namespace
{
// An X11 keycode is an evdev one with 8 added, on every server with an evdev
// or libinput driver and on XWayland.
constexpr uint32_t x11KeycodeOffset = 8;

// The wheel is buttons on core X11; 4/5 are one notch up and down, 6/7 one
// left and right. Smooth scrolling is XI2's, and stage 5's (plan.md).
constexpr uint8_t x11WheelUp = 4;
constexpr uint8_t x11WheelDown = 5;
constexpr uint8_t x11WheelLeft = 6;
constexpr uint8_t x11WheelRight = 7;

// Everything a locked pointer's grab has to keep reporting.
constexpr uint16_t x11PointerGrabMask =
    XCB_EVENT_MASK_BUTTON_PRESS | XCB_EVENT_MASK_BUTTON_RELEASE
    | XCB_EVENT_MASK_POINTER_MOTION | XCB_EVENT_MASK_ENTER_WINDOW
    | XCB_EVENT_MASK_LEAVE_WINDOW;

// The header's own union is behind the `explicit` field, so the four bytes
// every XKB event starts with are spelled out here instead.
struct X11XkbAnyEvent
{
    uint8_t responseType;
    uint8_t xkbType;
    uint16_t sequence;
    xcb_timestamp_t time;
    uint8_t deviceId;
};

template <typename T>
const T& x11InputAs(const xcb_generic_event_t& event)
{
    return *reinterpret_cast<const T*>(&event);
}

uint32_t x11EvdevFromButton(uint8_t button)
{
    switch (button)
    {
        case 1:
            return BTN_LEFT;
        case 2:
            return BTN_MIDDLE;
        case 3:
            return BTN_RIGHT;
        default:
            return 0;
    }
}

bool x11IsWheelButton(uint8_t button)
{
    return button >= x11WheelUp && button <= x11WheelRight;
}

// A crossing into or out of one of our own child windows is not the pointer
// leaving, and a grab taking it away is not either.
bool x11IsRealCrossing(uint8_t mode, uint8_t detail)
{
    return mode != XCB_NOTIFY_MODE_GRAB && mode != XCB_NOTIFY_MODE_UNGRAB
           && detail != XCB_NOTIFY_DETAIL_INFERIOR;
}
} // namespace

X11Input::X11Input(X11Connection& connectionToUse)
    : connection(connectionToUse)
    , deviceId(connectionToUse.getKeyboardDeviceId())
{
    wheel.setSource(false, false);

    if (deviceId < 0)
        return;

    selectXkbEvents();
    requestDetectableAutoRepeat();
    loadKeymap();
    readServerState();

    connection.onXkbEvent = [this](const xcb_generic_event_t& event)
    { handleXkbEvent(event); };
}

X11Input::~X11Input()
{
    connection.onXkbEvent = [](const xcb_generic_event_t&) {};

    disengageMouseLock();

    if (!connection.isConnected())
        return;

    for (const auto& cached: cursorCache)
        if (cached.cursor != XCB_CURSOR_NONE)
            xcb_free_cursor(xcb(), cached.cursor);
}

// The quickstart selection: enough of the map for a keymap rebuild and enough
// of the state for the modifiers and the layout group.
void X11Input::selectXkbEvents()
{
    constexpr uint16_t events = XCB_XKB_EVENT_TYPE_NEW_KEYBOARD_NOTIFY
                                | XCB_XKB_EVENT_TYPE_MAP_NOTIFY
                                | XCB_XKB_EVENT_TYPE_STATE_NOTIFY;

    constexpr uint16_t newKeyboardDetails = XCB_XKB_NKN_DETAIL_KEYCODES;

    constexpr uint16_t mapParts =
        XCB_XKB_MAP_PART_KEY_TYPES | XCB_XKB_MAP_PART_KEY_SYMS
        | XCB_XKB_MAP_PART_MODIFIER_MAP | XCB_XKB_MAP_PART_EXPLICIT_COMPONENTS
        | XCB_XKB_MAP_PART_KEY_ACTIONS | XCB_XKB_MAP_PART_VIRTUAL_MODS
        | XCB_XKB_MAP_PART_VIRTUAL_MOD_MAP;

    constexpr uint16_t stateDetails =
        XCB_XKB_STATE_PART_MODIFIER_BASE | XCB_XKB_STATE_PART_MODIFIER_LATCH
        | XCB_XKB_STATE_PART_MODIFIER_LOCK | XCB_XKB_STATE_PART_GROUP_BASE
        | XCB_XKB_STATE_PART_GROUP_LATCH | XCB_XKB_STATE_PART_GROUP_LOCK;

    auto details = xcb_xkb_select_events_details_t {};
    details.affectNewKeyboard = newKeyboardDetails;
    details.newKeyboardDetails = newKeyboardDetails;
    details.affectState = stateDetails;
    details.stateDetails = stateDetails;

    xcb_xkb_select_events_aux(xcb(),
                              (xcb_xkb_device_spec_t) deviceId,
                              events,
                              0,
                              0,
                              mapParts,
                              mapParts,
                              &details);
}

// The server is what repeats a held key on X11, at the rate its own controls
// name, and it goes on doing so whatever a client asks: KeyRepeat is Wayland's
// answer to a compositor that repeats nothing, and running it here as well
// would deliver every repeat twice. What detectable auto-repeat changes is the
// shape of the stream - without it every repeat arrives as a release and a
// press a client cannot tell from the user letting go and pressing again, and
// with it as a bare press of a key that never came up.
void X11Input::requestDetectableAutoRepeat()
{
    constexpr uint32_t flag = XCB_XKB_PER_CLIENT_FLAG_DETECTABLE_AUTO_REPEAT;

    auto* reply = xcb_xkb_per_client_flags_reply(
        xcb(),
        xcb_xkb_per_client_flags(
            xcb(), (xcb_xkb_device_spec_t) deviceId, flag, flag, 0, 0, 0),
        nullptr);

    if (reply == nullptr)
        return;

    detectableAutoRepeat = (reply->value & flag) != 0;
    std::free(reply);

    if (!detectableAutoRepeat)
        LOG("X11: the server would not turn on detectable auto-repeat, so a "
            "held key arrives as release/press pairs and KeyEvent::isRepeat "
            "will read false on every one of them.");
}

void X11Input::loadKeymap()
{
    auto* newKeymap = xkb_x11_keymap_new_from_device(
        keyboardState.getContext(), xcb(), deviceId, XKB_KEYMAP_COMPILE_NO_FLAGS);

    if (newKeymap == nullptr)
        return;

    keyboardState.setKeymap(newKeymap);
}

// A fresh keymap brings a fresh state with it, so whatever is latched or
// locked right now has to be asked for rather than waited for.
void X11Input::readServerState()
{
    auto* reply = xcb_xkb_get_state_reply(
        xcb(), xcb_xkb_get_state(xcb(), (xcb_xkb_device_spec_t) deviceId), nullptr);

    if (reply == nullptr)
        return;

    keyboardState.setModifiers(
        reply->baseMods, reply->latchedMods, reply->lockedMods, reply->lockedGroup);

    std::free(reply);
}

void X11Input::handleXkbEvent(const xcb_generic_event_t& event)
{
    const auto& any = x11InputAs<X11XkbAnyEvent>(event);

    if ((int32_t) any.deviceId != deviceId)
        return;

    switch (any.xkbType)
    {
        case XCB_XKB_NEW_KEYBOARD_NOTIFY:
        case XCB_XKB_MAP_NOTIFY:
            loadKeymap();
            readServerState();
            break;

        case XCB_XKB_STATE_NOTIFY:
        {
            const auto& state = x11InputAs<xcb_xkb_state_notify_event_t>(event);

            keyboardState.setModifiers(state.baseMods,
                                       state.latchedMods,
                                       state.lockedMods,
                                       state.lockedGroup);
            break;
        }

        default:
            break;
    }
}

void X11Input::keyChanged(X11WindowSurface& window,
                          const xcb_key_press_event_t& event,
                          bool pressed)
{
    if (keyboardWindow != &window)
        return;

    if (event.detail < x11KeycodeOffset)
        return;

    const auto code = (uint32_t) event.detail - x11KeycodeOffset;

    keyTime = event.time;

    // A press of a key that never came up is one of the server's repeats,
    // which is the only form a repeat takes here.
    const auto isRepeat = pressed && keyboardState.isPressed(code);

    keyboardState.setPressed(code, pressed);

    deliverKey(code, pressed, isRepeat);
}

void X11Input::deliverKey(uint32_t code, bool down, bool isRepeat)
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

// A grab is a menu or a drag taking the keyboard for a moment, and
// NotifyPointer is the pointer crossing; neither is the window losing the
// user's attention. Dropping NotifyPointer is what makes focus-on-click the
// rule on a server with no window manager, where the root's focus is
// PointerRoot and every crossing would otherwise hand a window the keyboard:
// keys reach a window there only once something has clicked it.
void X11Input::focusChanged(X11WindowSurface& window,
                            const xcb_focus_in_event_t& event,
                            bool focused)
{
    if (event.mode == XCB_NOTIFY_MODE_GRAB || event.mode == XCB_NOTIFY_MODE_UNGRAB
        || event.detail == XCB_NOTIFY_DETAIL_POINTER)
        return;

    if (focused)
        setKeyboardFocus(&window);
    else if (keyboardWindow == &window)
        setKeyboardFocus(nullptr);
}

void X11Input::setKeyboardFocus(X11WindowSurface* window)
{
    if (keyboardWindow == window)
        return;

    auto* previous = keyboardWindow;
    keyboardWindow = window;

    // The matching key-ups go to whoever took focus, so a state kept across
    // the change would report keys stuck down forever.
    keyboardState.clearPressed();

    // The lock goes before the callback rather than after it: letting go of
    // one asks nothing of the window, and onKeyboardFocus reaches the app's
    // onActivationChanged, which is allowed to destroy the Window it names.
    if (previous != nullptr)
    {
        if (lockedWindow == previous)
            disengageMouseLock();

        previous->onKeyboardFocus(false);
    }

    // Re-read after every callback for the same reason: a window destroyed
    // from inside one takes itself out of here through windowDestroyed.
    if (auto* gained = keyboardWindow; gained != nullptr && gained == window)
    {
        gained->onKeyboardFocus(true);

        if (keyboardWindow == gained)
            updateMouseLock(*gained);
    }
}

void X11Input::pointerEntered(X11WindowSurface& window,
                              const xcb_enter_notify_event_t& event)
{
    if (!x11IsRealCrossing(event.mode, event.detail))
        return;

    setPointerWindow(&window);
    pointerState.setPosition({(float) event.event_x, (float) event.event_y});

    cursor.setHidden(lockedWindow == &window);

    refreshCursor();
    applyCursor();

    auto moved = MouseEvent {};
    moved.type = MouseEventType::Moved;
    moved.pos = pointerState.getPosition();
    moved.modifiers = getModifiers();
    moved.timestamp = linuxTimestamp(event.time);

    dispatchMouse(moved);
}

void X11Input::pointerLeft(X11WindowSurface& window,
                           const xcb_leave_notify_event_t& event)
{
    if (pointerWindow != &window || !x11IsRealCrossing(event.mode, event.detail))
        return;

    if (window.contentView != nullptr)
    {
        auto exited = MouseEvent {};
        exited.type = MouseEventType::Exited;
        exited.pos = pointerState.getPosition();
        exited.modifiers = getModifiers();
        exited.timestamp = linuxTimestamp(event.time);

        window.contentView->dispatchMouseEvent(exited);
    }

    setPointerWindow(nullptr);
}

void X11Input::pointerMoved(X11WindowSurface& window,
                            const xcb_motion_notify_event_t& event)
{
    // A grab keeps reporting motion after the enter that would have named the
    // window; a pointer over one of our windows is in it whatever happened.
    if (pointerWindow != &window)
        setPointerWindow(&window);

    const auto moved = Point {(float) event.event_x, (float) event.event_y};

    auto delta = Point {};

    if (lockedWindow == &window)
    {
        const auto centre = lockCentre();

        delta = {moved.x - centre.x, moved.y - centre.y};

        // The warp's own motion, which is not the hand's.
        if (delta.x == 0.f && delta.y == 0.f)
            return;

        warpToLockCentre();
    }
    else
    {
        const auto previous = pointerState.getPosition();

        delta = {moved.x - previous.x, moved.y - previous.y};
        pointerState.setPosition(moved);
    }

    auto dispatched = MouseEvent {};
    dispatched.pos = pointerState.getPosition();
    dispatched.delta = delta;
    dispatched.rawDelta = delta;
    dispatched.button = pointerState.getHeldButton();
    dispatched.modifiers = getModifiers();
    dispatched.clickCount = pointerState.getClickCount();
    dispatched.timestamp = linuxTimestamp(event.time);

    // Only Dragged and Up go to the view that captured the mouse down; a plain
    // Moved is re-hit-tested, and would lose a grab in progress.
    dispatched.type = pointerState.isButtonHeld() ? MouseEventType::Dragged
                                                  : MouseEventType::Moved;

    dispatchMouse(dispatched);
    refreshCursor();
}

void X11Input::buttonChanged(X11WindowSurface& window,
                             const xcb_button_press_event_t& event,
                             bool pressed)
{
    if (pointerWindow != &window)
        setPointerWindow(&window);

    if (lockedWindow != &window)
        pointerState.setPosition({(float) event.event_x, (float) event.event_y});

    if (x11IsWheelButton(event.detail))
    {
        // The release of a wheel button is the same notch reported twice.
        if (pressed)
        {
            wheelFromButton(event.detail);
            dispatchWheel(event.time);
        }

        return;
    }

    if (pressed)
        takeFocusOnClick(window, event.time);

    const auto code = x11EvdevFromButton(event.detail);

    // Back and forward have no MouseButton to land on.
    if (code == 0)
        return;

    auto dispatched = MouseEvent {};
    dispatched.pos = pointerState.getPosition();
    dispatched.button = linuxButtonFromEvdev(code);
    dispatched.modifiers = getModifiers();
    dispatched.timestamp = linuxTimestamp(event.time);

    if (pressed)
    {
        dispatched.type = MouseEventType::Down;
        dispatched.clickCount = pointerState.pressed(dispatched.button, event.time);
    }
    else
    {
        pointerState.released();

        dispatched.type = MouseEventType::Up;
        dispatched.clickCount = pointerState.getClickCount();
    }

    dispatchMouse(dispatched);
}

// MouseEvent::delta is positive upwards and leftwards, which is the opposite
// of the direction each button names.
void X11Input::wheelFromButton(uint8_t button)
{
    switch (button)
    {
        case x11WheelUp:
            wheel.addNotches({0.f, 1.f});
            break;
        case x11WheelDown:
            wheel.addNotches({0.f, -1.f});
            break;
        case x11WheelLeft:
            wheel.addNotches({1.f, 0.f});
            break;
        case x11WheelRight:
            wheel.addNotches({-1.f, 0.f});
            break;
        default:
            break;
    }
}

// No frames on X11: a notch is dispatched as it arrives.
void X11Input::dispatchWheel(uint32_t time)
{
    if (!wheel.isPending())
        return;

    auto event = MouseEvent {};
    event.type = MouseEventType::Wheel;
    event.pos = pointerState.getPosition();
    event.downPos = event.pos;
    event.modifiers = getModifiers();
    event.preciseScrolling = wheel.isPrecise();
    event.timestamp = linuxTimestamp(time);
    event.delta = wheel.getDelta();

    wheel.endFrame();

    if (pointerWindow == nullptr || pointerWindow->contentView == nullptr)
        return;

    if (event.delta.x != 0.f || event.delta.y != 0.f)
        pointerWindow->contentView->dispatchMouseEvent(event);
}

void X11Input::dispatchMouse(MouseEvent event)
{
    if (pointerWindow == nullptr || pointerWindow->contentView == nullptr)
        return;

    event.downPos = event.type == MouseEventType::Wheel
                        ? event.pos
                        : pointerState.getDownPosition();

    pointerWindow->contentView->dispatchMouseEvent(event);
}

void X11Input::setPointerWindow(X11WindowSurface* window)
{
    if (pointerWindow == window)
        return;

    // Whatever shape the old window was wearing is not this one's.
    if (cursor.setHidden(false))
        applyCursor();

    pointerWindow = window;
}

// A locked pointer has no shape: it is hidden until the lock lets go, and
// asking the view under it would only put the arrow back.
void X11Input::refreshCursor()
{
    if (lockedWindow != nullptr || pointerWindow == nullptr
        || pointerWindow->contentView == nullptr)
        return;

    auto* contentView = pointerWindow->contentView;
    auto* hit = contentView->hitTest(pointerState.getPosition());
    auto shape =
        hit != nullptr ? hit->getMouseCursor() : contentView->getMouseCursor();

    if (cursor.setShape(shape))
        applyCursor();
}

void X11Input::applyCursor()
{
    if (pointerWindow == nullptr || pointerWindow->getWindow() == XCB_NONE
        || !connection.isConnected())
        return;

    const auto window = pointerWindow->getWindow();

    // Hide and show are reference counted per client, so a show with nothing
    // hidden is an error rather than a no-op.
    if (cursor.isHidden() != cursorHidden)
    {
        cursorHidden = cursor.isHidden();

        if (cursorHidden)
            xcb_xfixes_hide_cursor(xcb(), window);
        else
            xcb_xfixes_show_cursor(xcb(), window);
    }

    if (!cursorHidden)
    {
        const uint32_t shape = cursorForShape(cursor.getShape());
        xcb_change_window_attributes(xcb(), window, XCB_CW_CURSOR, &shape);
    }

    connection.flush();
}

// One cursor per shape for the life of the connection: loading one is a round
// trip through the theme, and a pointer crossing a few views would repeat it
// several times a second.
xcb_cursor_t X11Input::cursorForShape(MouseCursor shape)
{
    for (const auto& cached: cursorCache)
        if (cached.shape == shape)
            return cached.cursor;

    auto loaded = xcb_cursor_t {XCB_CURSOR_NONE};

    if (auto* context = connection.getCursorContext())
    {
        for (const auto* name: linuxCursorNames(shape))
        {
            loaded = xcb_cursor_load_cursor(context, name);

            if (loaded != XCB_CURSOR_NONE)
                break;
        }
    }

    cursorCache.add({shape, loaded});

    return loaded;
}

// Hosts keep focus on their own window and forward nothing, so a click inside
// is the only thing that ever gives an embedded child the keyboard (plan.md
// D6). Harmless on a toplevel a window manager is already focusing.
void X11Input::takeFocusOnClick(X11WindowSurface& window, xcb_timestamp_t time)
{
    if (keyboardWindow == &window || !window.mapped || window.getWindow() == XCB_NONE
        || !connection.isConnected())
        return;

    xcb_set_input_focus(xcb(), XCB_INPUT_FOCUS_PARENT, window.getWindow(), time);
    connection.flush();
}

void X11Input::updateMouseLock(X11WindowSurface& window)
{
    const auto wanted =
        window.mouseLockIntent && keyboardWindow == &window && window.mapped;

    if (wanted)
        engageMouseLock(window);
    else if (lockedWindow == &window)
        disengageMouseLock();
}

// The grab confines the pointer to the window, the warp pins it in the middle
// and every motion from there is a delta the warp then undoes.
void X11Input::engageMouseLock(X11WindowSurface& window)
{
    if (lockedWindow != nullptr || window.getWindow() == XCB_NONE
        || !connection.isConnected())
        return;

    auto* reply = xcb_grab_pointer_reply(xcb(),
                                         xcb_grab_pointer(xcb(),
                                                          1,
                                                          window.getWindow(),
                                                          x11PointerGrabMask,
                                                          XCB_GRAB_MODE_ASYNC,
                                                          XCB_GRAB_MODE_ASYNC,
                                                          window.getWindow(),
                                                          XCB_CURSOR_NONE,
                                                          XCB_CURRENT_TIME),
                                         nullptr);

    if (reply == nullptr)
        return;

    const auto granted = reply->status == XCB_GRAB_STATUS_SUCCESS;
    std::free(reply);

    if (!granted)
        return;

    lockedWindow = &window;

    setPointerWindow(&window);

    cursor.setHidden(true);
    applyCursor();

    warpToLockCentre();
}

void X11Input::disengageMouseLock()
{
    if (lockedWindow == nullptr)
        return;

    lockedWindow = nullptr;

    if (connection.isConnected())
        xcb_ungrab_pointer(xcb(), XCB_CURRENT_TIME);

    if (cursor.setHidden(false))
        applyCursor();
    else if (connection.isConnected())
        connection.flush();
}

Point X11Input::lockCentre() const
{
    if (lockedWindow == nullptr)
        return {};

    return {std::floor(lockedWindow->contentSize.x / 2.f),
            std::floor(lockedWindow->contentSize.y / 2.f)};
}

void X11Input::warpToLockCentre()
{
    if (lockedWindow == nullptr || !connection.isConnected())
        return;

    const auto centre = lockCentre();

    xcb_warp_pointer(xcb(),
                     XCB_NONE,
                     lockedWindow->getWindow(),
                     0,
                     0,
                     0,
                     0,
                     (int16_t) centre.x,
                     (int16_t) centre.y);
    connection.flush();
}

bool X11Input::isKeyPressed(uint32_t evdevCode) const
{
    return keyboardState.isPressed(evdevCode);
}

Vector<uint32_t> X11Input::getPressedCodes() const
{
    return keyboardState.getPressedCodes();
}

ModifierKeys X11Input::getModifiers() const
{
    return keyboardState.getModifiers();
}

std::string X11Input::characterForCode(uint32_t evdevCode) const
{
    return keyboardState.plainTextForKey(evdevCode);
}

void X11Input::windowDestroyed(X11WindowSurface& window)
{
    if (lockedWindow == &window)
        disengageMouseLock();

    if (keyboardWindow == &window)
    {
        keyboardState.clearPressed();
        keyboardWindow = nullptr;
    }

    if (pointerWindow == &window)
        setPointerWindow(nullptr);
}
} // namespace eacp::Graphics
