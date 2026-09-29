#include "Common.h"

#include <eacp/Core/Threads/EventLoop.h>
#include <eacp/Core/Utils/Environment.h>
#include <eacp/Graphics/Window/LinuxInput-Linux.h>
#include <eacp/Graphics/Window/LinuxWindowSystem-Linux.h>

#include <xcb/xcb.h>

#if EACP_HAS_XTEST
#include <xcb/xtest.h>
#endif

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <linux/input-event-codes.h>
#include <memory>
#include <optional>
#include <source_location>
#include <string_view>

// WindowOptions::popup on X11: the menu a host opens over a plugin's editor.
//
// What makes one work is invisible to anything portable. It is an
// override-redirect window, so nothing places or stacks it but us; it is
// transient for the window it pops over, so a window manager keeps the two
// together; it never takes the keyboard, so the window underneath does not
// grey out; and it holds the pointer, so a press outside closes it instead of
// reaching whatever it landed on - which may be a host's own window, and no
// window of ours to decide for.
//
// Every case self-skips without a display, as the rest of the X11 suite does,
// and the ones that drive the pointer and the keyboard skip again on Xwayland,
// whose seat belongs to the compositor. Scripts/with-xvfb is where they run for
// real, which is what CI does.

using namespace nano;
using namespace eacp;
using namespace eacp::Graphics;

namespace
{
constexpr auto x11PopupTestTimeout = Time::MS {5000};

// check() reports and carries on, which is what a suite wants - but a case
// that then reads the reply it has just failed to find would take the process
// down with it instead of failing.
bool x11PopupArrived(
    bool condition,
    std::string_view message = {},
    const std::source_location& location = std::source_location::current())
{
    check(condition, message, location);

    return condition;
}

bool x11PopupServerReachable()
{
    static const auto reachable = []
    {
        if (Apps::getAppEnvironment().headless)
            return false;

        if (getEnvValue("EACP_WINDOW_SYSTEM") != "x11")
            return false;

        if (getEnvValue("DISPLAY").empty())
            return false;

        auto probe = Window {WindowOptions {}};

        return probe.getHandle() != nullptr;
    }();

    return reachable;
}

xcb_window_t x11PopupIdOf(Window& window)
{
    return (xcb_window_t) (uintptr_t) window.getHandle();
}

// A connection of the test's own, so what is asserted about a window comes
// from the server rather than from the backend under test.
struct X11PopupConnection
{
    X11PopupConnection()
        : connection(xcb_connect(nullptr, nullptr))
    {
        if (xcb_connection_has_error(connection) != 0)
        {
            xcb_disconnect(connection);
            connection = nullptr;
        }
    }

    ~X11PopupConnection()
    {
        if (connection != nullptr)
            xcb_disconnect(connection);
    }

    bool isValid() const { return connection != nullptr; }

    xcb_window_t root() const
    {
        return xcb_setup_roots_iterator(xcb_get_setup(connection)).data->root;
    }

    xcb_connection_t* connection = nullptr;
};

xcb_atom_t x11PopupAtom(xcb_connection_t* probe, const char* name)
{
    auto* reply = xcb_intern_atom_reply(
        probe,
        xcb_intern_atom(probe, 0, (uint16_t) std::strlen(name), name),
        nullptr);

    if (reply == nullptr)
        return XCB_ATOM_NONE;

    const auto atom = reply->atom;
    std::free(reply);

    return atom;
}

// The first 32-bit word of a property, and nothing where the window carries
// none.
std::optional<uint32_t> x11PopupWordProperty(xcb_connection_t* probe,
                                             xcb_window_t window,
                                             xcb_atom_t property)
{
    if (property == XCB_ATOM_NONE)
        return {};

    auto* reply = xcb_get_property_reply(
        probe,
        xcb_get_property(
            probe, 0, window, property, XCB_GET_PROPERTY_TYPE_ANY, 0, 4),
        nullptr);

    if (reply == nullptr)
        return {};

    auto value = std::optional<uint32_t> {};

    if (reply->format == 32 && xcb_get_property_value_length(reply) >= 4)
        value = *static_cast<const uint32_t*>(xcb_get_property_value(reply));

    std::free(reply);

    return value;
}

std::optional<uint32_t> x11PopupWindowType(xcb_connection_t* probe,
                                           xcb_window_t window)
{
    return x11PopupWordProperty(
        probe, window, x11PopupAtom(probe, "_NET_WM_WINDOW_TYPE"));
}

bool x11PopupIsOverrideRedirect(xcb_connection_t* probe, xcb_window_t window)
{
    auto* reply = xcb_get_window_attributes_reply(
        probe, xcb_get_window_attributes(probe, window), nullptr);

    if (reply == nullptr)
        return false;

    const auto redirected = reply->override_redirect != 0;
    std::free(reply);

    return redirected;
}

xcb_window_t x11PopupParentOf(xcb_connection_t* probe, xcb_window_t window)
{
    auto* reply =
        xcb_query_tree_reply(probe, xcb_query_tree(probe, window), nullptr);

    if (reply == nullptr)
        return XCB_NONE;

    const auto parent = reply->parent;
    std::free(reply);

    return parent;
}

xcb_window_t x11PopupFocusedWindow(xcb_connection_t* probe)
{
    auto* reply =
        xcb_get_input_focus_reply(probe, xcb_get_input_focus(probe), nullptr);

    if (reply == nullptr)
        return XCB_NONE;

    const auto focused = reply->focus;
    std::free(reply);

    return focused;
}

// Where the window's top left really is, whatever frame a window manager put
// around it, so a local point can be aimed at in root coordinates.
Point x11PopupRootOriginOf(xcb_connection_t* probe, xcb_window_t window)
{
    const auto root = xcb_setup_roots_iterator(xcb_get_setup(probe)).data->root;

    auto* reply = xcb_translate_coordinates_reply(
        probe, xcb_translate_coordinates(probe, window, root, 0, 0), nullptr);

    if (reply == nullptr)
        return {};

    const auto origin = Point {(float) reply->dst_x, (float) reply->dst_y};
    std::free(reply);

    return origin;
}

// Whether the pointer is free, asked as another client: the only thing that
// says from outside this process whether a grab is really held.
bool x11PopupPointerIsFree(const X11PopupConnection& grabber)
{
    auto* probe = grabber.connection;

    auto* reply = xcb_grab_pointer_reply(probe,
                                         xcb_grab_pointer(probe,
                                                          0,
                                                          grabber.root(),
                                                          0,
                                                          XCB_GRAB_MODE_ASYNC,
                                                          XCB_GRAB_MODE_ASYNC,
                                                          XCB_NONE,
                                                          XCB_CURSOR_NONE,
                                                          XCB_CURRENT_TIME),
                                         nullptr);

    if (reply == nullptr)
        return false;

    const auto granted = reply->status == XCB_GRAB_STATUS_SUCCESS;
    std::free(reply);

    if (granted)
    {
        xcb_ungrab_pointer(probe, XCB_CURRENT_TIME);
        xcb_flush(probe);
    }

    return granted;
}

// A view that remembers what it was told, and nothing else.
struct X11PopupRecordingView : View
{
    X11PopupRecordingView() { getProperties().handlesMouseEvents = true; }

    void mouseDown(const MouseEvent&) override { ++downs; }
    void mouseUp(const MouseEvent&) override { ++ups; }

    void forget()
    {
        downs = 0;
        ups = 0;
    }

    int downs = 0;
    int ups = 0;
};

// Every other suite's windows come up at the origin, and a display has one
// stack: a window over this one would take the presses meant for it.
Point x11PopupOwnerOrigin(int width, int height)
{
    const auto display = primaryDisplay().frame;

    return {std::max(display.w - (float) width - 60.f, 0.f),
            std::max(display.h - (float) height - 60.f, 0.f)};
}

// The window every case pops a menu over: small, out of the way, and with a
// view that records whatever press reaches it.
struct X11PopupOwner
{
    X11PopupOwner(int width = 400, int height = 300)
    {
        auto options = WindowOptions {};
        options.width = width;
        options.height = height;
        options.title = "eacp X11 popup tests";
        options.isPrimary = false;
        options.initialPosition = x11PopupOwnerOrigin(width, height);

        window.emplace(options);

        window->events.onActivationChanged = [this](bool nowActive)
        { active = nowActive; };

        window->setContentView(content);

        Threads::runEventLoopUntil([this] { return window->isVisible(); },
                                   x11PopupTestTimeout);
    }

    bool isUp() { return window.has_value() && window->isVisible(); }

    xcb_window_t id() { return x11PopupIdOf(*window); }

    X11PopupRecordingView content;
    bool active = false;
    std::optional<Window> window;
};

WindowOptions x11PopupOptions(Window& owner, Point offset)
{
    auto options = WindowOptions {};
    options.popup = true;
    options.parent = &owner;
    options.width = 160;
    options.height = 120;
    options.initialPosition = owner.getPosition() + offset;

    return options;
}

// The menu itself, with a view of its own: a press inside one has to reach it,
// or no item could ever be chosen.
struct X11PopupWindow
{
    explicit X11PopupWindow(const WindowOptions& options)
    {
        window.emplace(options);

        window->events.onDismissRequested = [this] { ++dismissals; };

        window->setContentView(content);

        Threads::runEventLoopUntil([this] { return window->isVisible(); },
                                   x11PopupTestTimeout);
    }

    bool isUp() { return window.has_value() && window->isVisible(); }

    xcb_window_t id() { return x11PopupIdOf(*window); }

    X11PopupRecordingView content;
    int dismissals = 0;
    std::optional<Window> window;
};

// XTest against the test's own connection: the server's real pointer and
// keyboard, so what the backend sees is indistinguishable from a device.
struct X11PopupInput
{
    explicit X11PopupInput(xcb_connection_t* connectionToUse)
        : connection(connectionToUse)
    {
#if EACP_HAS_XTEST
        if (connection == nullptr)
            return;

        const auto* extension = xcb_get_extension_data(connection, &xcb_test_id);

        available = extension != nullptr && extension->present != 0;
        root = xcb_setup_roots_iterator(xcb_get_setup(connection)).data->root;
#endif
    }

    // A warp rather than XTest's own motion: Xwayland accepts a fake motion
    // and does nothing with it, while WarpPointer moves the pointer on both
    // servers and generates the MotionNotify a hand would.
    void moveTo(Point rootPosition)
    {
        if (!available)
            return;

        xcb_warp_pointer(connection,
                         XCB_NONE,
                         root,
                         0,
                         0,
                         0,
                         0,
                         (int16_t) rootPosition.x,
                         (int16_t) rootPosition.y);
        xcb_flush(connection);
    }

    void click(uint8_t number)
    {
        fake(XCB_BUTTON_PRESS, number);
        fake(XCB_BUTTON_RELEASE, number);
    }

    // The evdev code the framework speaks, turned back into what the server
    // wants: X11 keycodes are evdev + 8.
    void key(uint32_t evdevCode, bool pressed)
    {
        fake(pressed ? XCB_KEY_PRESS : XCB_KEY_RELEASE, (uint8_t) (evdevCode + 8));
    }

    void fake(uint8_t type, uint8_t detail)
    {
#if EACP_HAS_XTEST
        if (!available)
            return;

        xcb_test_fake_input(
            connection, type, detail, XCB_CURRENT_TIME, root, 0, 0, 0);
        xcb_flush(connection);
#else
        (void) type;
        (void) detail;
#endif
    }

    bool available = false;
    xcb_window_t root = XCB_NONE;
    xcb_connection_t* connection = nullptr;
};

// Under Xwayland the pointer and the keyboard belong to the compositor: an X
// client's synthetic input moves the server's own idea of where the pointer is
// without the compositor ever hearing about it. The extension is on no other
// server, which makes it the test.
bool x11PopupServerOwnsItsSeat(xcb_connection_t* probe)
{
    auto* reply = xcb_query_extension_reply(
        probe, xcb_query_extension(probe, 8, "XWAYLAND"), nullptr);

    const auto compositorOwned = reply != nullptr && reply->present != 0;
    std::free(reply);

    return !compositorOwned;
}

bool x11PopupCanDriveTheSeat(xcb_connection_t* probe, const X11PopupInput& input)
{
    if (!input.available)
    {
        LOG("XTest is not available on this server: the popup input cases "
            "cannot synthesise a pointer or a key, and are skipped.");

        return false;
    }

    if (!x11PopupServerOwnsItsSeat(probe))
    {
        LOG("This is an Xwayland server, whose pointer and keyboard the "
            "compositor owns, so the popup input cases are skipped. "
            "Scripts/with-xvfb runs them for real, which is what CI does.");

        return false;
    }

    return true;
}

// Nothing else on this display may be over the window a case is aiming at, and
// there are two ways to say so: with no window manager the stack is the
// client's own to change, and with one the request that counts is
// _NET_WM_STATE_ABOVE.
void x11PopupRaise(xcb_connection_t* probe, xcb_window_t window)
{
    const uint32_t above = XCB_STACK_MODE_ABOVE;

    xcb_configure_window(probe, window, XCB_CONFIG_WINDOW_STACK_MODE, &above);

    const auto root = xcb_setup_roots_iterator(xcb_get_setup(probe)).data->root;

    auto event = xcb_client_message_event_t {};
    event.response_type = XCB_CLIENT_MESSAGE;
    event.format = 32;
    event.window = window;
    event.type = x11PopupAtom(probe, "_NET_WM_STATE");

    event.data.data32[0] = 1;
    event.data.data32[1] = x11PopupAtom(probe, "_NET_WM_STATE_ABOVE");
    event.data.data32[3] = 1;

    xcb_send_event(probe,
                   0,
                   root,
                   XCB_EVENT_MASK_SUBSTRUCTURE_NOTIFY
                       | XCB_EVENT_MASK_SUBSTRUCTURE_REDIRECT,
                   reinterpret_cast<const char*>(&event));
    xcb_flush(probe);

    Threads::runEventLoopFor(Time::MS {150});
}

// The keyboard on the window the menu will pop over, with no click to stack
// against and no window manager policy in the way.
bool x11PopupFocusOwner(xcb_connection_t* probe, X11PopupOwner& owner)
{
    x11PopupRaise(probe, owner.id());

    xcb_set_input_focus(probe, XCB_INPUT_FOCUS_PARENT, owner.id(), XCB_CURRENT_TIME);
    xcb_flush(probe);

    Threads::runEventLoopUntil([&] { return owner.active; }, x11PopupTestTimeout);

    owner.content.forget();

    return owner.active;
}
} // namespace

// Nothing places, stacks or resizes a popup but the app that opened it, which
// is what override-redirect says and the only way a menu opens exactly where
// the click was.
auto tX11PopupIsOverrideRedirect = test("X11/popupIsAnOverrideRedirectWindow") = []
{
    if (!x11PopupServerReachable())
        return;

    auto owner = X11PopupOwner {};
    auto popup = X11PopupWindow {x11PopupOptions(*owner.window, {200.f, 150.f})};
    auto probe = X11PopupConnection {};

    check(probe.isValid());
    check(owner.isUp());
    check(popup.isUp(), "the popup never mapped");

    check(x11PopupIsOverrideRedirect(probe.connection, popup.id()),
          "the popup is an ordinary managed window");

    // And an ordinary window is left as it was, or every window would place
    // itself and no window manager would ever see one.
    check(!x11PopupIsOverrideRedirect(probe.connection, owner.id()));
};

// What tells a window manager which window this one belongs to, so it is kept
// above it and taken down with it.
auto tX11PopupIsTransientForItsOwner = test("X11/popupIsTransientForItsOwner") = []
{
    if (!x11PopupServerReachable())
        return;

    auto owner = X11PopupOwner {};
    auto popup = X11PopupWindow {x11PopupOptions(*owner.window, {200.f, 150.f})};
    auto probe = X11PopupConnection {};

    check(probe.isValid());

    const auto transient = x11PopupWordProperty(
        probe.connection, popup.id(), XCB_ATOM_WM_TRANSIENT_FOR);

    if (!x11PopupArrived(transient.has_value(),
                         "the popup carries no WM_TRANSIENT_FOR"))
        return;

    check(*transient == owner.id(),
          "WM_TRANSIENT_FOR does not name the window the popup pops over");
};

// The type a desktop reads to decide a window's shadow, its animation and
// whether it belongs in the task list.
auto tX11PopupIsAPopupMenu = test("X11/popupIsTypedAsAPopupMenu") = []
{
    if (!x11PopupServerReachable())
        return;

    auto owner = X11PopupOwner {};
    auto popup = X11PopupWindow {x11PopupOptions(*owner.window, {200.f, 150.f})};
    auto probe = X11PopupConnection {};

    check(probe.isValid());

    const auto type = x11PopupWindowType(probe.connection, popup.id());

    if (!x11PopupArrived(type.has_value(),
                         "the popup carries no _NET_WM_WINDOW_TYPE"))
        return;

    check(*type == x11PopupAtom(probe.connection, "_NET_WM_WINDOW_TYPE_POPUP_MENU"),
          "the popup is typed as an ordinary window");

    const auto ownerType = x11PopupWindowType(probe.connection, owner.id());

    if (x11PopupArrived(ownerType.has_value()))
        check(*ownerType
              == x11PopupAtom(probe.connection, "_NET_WM_WINDOW_TYPE_NORMAL"));
};

// The whole point of it. A menu that takes the keyboard greys out the window
// it belongs to, which is how it reads as a window of its own rather than as
// part of the one underneath.
auto tX11PopupOwnerKeepsFocus = test("X11/popupLeavesTheOwnerFocused") = []
{
    if (!x11PopupServerReachable())
        return;

    auto owner = X11PopupOwner {};
    auto probe = X11PopupConnection {};

    check(probe.isValid());

    if (!x11PopupFocusOwner(probe.connection, owner))
        return;

    auto popup = X11PopupWindow {x11PopupOptions(*owner.window, {200.f, 150.f})};

    check(popup.isUp());

    check(x11PopupFocusedWindow(probe.connection) == owner.id(),
          "the popup took the keyboard from the window it pops over");
    check(owner.active, "the owner was told it had stopped being active");
};

// A press outside asks for the dismissal and is eaten on the way: clicking a
// menu away never also presses what was under it, which may be a host's own
// window and none of ours to decide for.
auto tX11PopupOutsidePressDismisses =
    test("X11/popupOutsidePressAsksForDismissal") = []
{
    if (!x11PopupServerReachable())
        return;

    auto probe = X11PopupConnection {};
    check(probe.isValid());

    auto input = X11PopupInput {probe.connection};

    if (!x11PopupCanDriveTheSeat(probe.connection, input))
        return;

    auto owner = X11PopupOwner {};

    if (!x11PopupFocusOwner(probe.connection, owner))
        return;

    const auto origin = x11PopupRootOriginOf(probe.connection, owner.id());

    {
        auto popup = X11PopupWindow {x11PopupOptions(*owner.window, {200.f, 150.f})};

        check(popup.isUp());

        input.moveTo(origin + Point {40.f, 40.f});
        Threads::runEventLoopFor(Time::MS {50});

        input.click(1);

        Threads::runEventLoopUntil([&] { return popup.dismissals > 0; },
                                   x11PopupTestTimeout);

        check(popup.dismissals == 1,
              "a press outside the popup did not ask for it to be dismissed");
    }

    check(owner.content.downs == 0,
          "the press that closed the menu also reached the view under it");
    check(owner.content.ups == 0);
};

// A press inside is the menu's own and must reach it, or no item could ever be
// chosen.
auto tX11PopupInsidePressIsDelivered =
    test("X11/popupInsidePressIsNotADismissal") = []
{
    if (!x11PopupServerReachable())
        return;

    auto probe = X11PopupConnection {};
    check(probe.isValid());

    auto input = X11PopupInput {probe.connection};

    if (!x11PopupCanDriveTheSeat(probe.connection, input))
        return;

    auto owner = X11PopupOwner {};

    if (!x11PopupFocusOwner(probe.connection, owner))
        return;

    auto popup = X11PopupWindow {x11PopupOptions(*owner.window, {200.f, 150.f})};

    check(popup.isUp());

    const auto origin = x11PopupRootOriginOf(probe.connection, popup.id());

    input.moveTo(origin + Point {30.f, 30.f});
    Threads::runEventLoopFor(Time::MS {50});

    input.click(1);

    Threads::runEventLoopUntil([&] { return popup.content.ups > 0; },
                               x11PopupTestTimeout);

    check(popup.dismissals == 0, "a press inside the popup dismissed it");
    check(popup.content.downs == 1, "the press never reached the popup's view");
};

// Escape, swallowed for the same reason and reaching the menu although the
// keyboard still belongs to the window underneath.
auto tX11PopupEscapeDismisses = test("X11/popupEscapeAsksForDismissal") = []
{
    if (!x11PopupServerReachable())
        return;

    auto probe = X11PopupConnection {};
    check(probe.isValid());

    auto input = X11PopupInput {probe.connection};

    if (!x11PopupCanDriveTheSeat(probe.connection, input))
        return;

    auto owner = X11PopupOwner {};

    if (!x11PopupFocusOwner(probe.connection, owner))
        return;

    auto popup = X11PopupWindow {x11PopupOptions(*owner.window, {200.f, 150.f})};

    check(popup.isUp());

    input.key(KEY_ESC, true);
    input.key(KEY_ESC, false);

    Threads::runEventLoopUntil([&] { return popup.dismissals > 0; },
                               x11PopupTestTimeout);

    check(popup.dismissals == 1, "Escape did not ask for the popup to be dismissed");
};

// The option covers the outside press and nothing else: a tooltip the app
// takes down itself keeps the click that landed behind it.
auto tX11PopupMayKeepTheOutsidePress =
    test("X11/popupWithoutDismissOnOutsideClickKeepsThePress") = []
{
    if (!x11PopupServerReachable())
        return;

    auto probe = X11PopupConnection {};
    check(probe.isValid());

    auto input = X11PopupInput {probe.connection};

    if (!x11PopupCanDriveTheSeat(probe.connection, input))
        return;

    auto owner = X11PopupOwner {};

    if (!x11PopupFocusOwner(probe.connection, owner))
        return;

    auto options = x11PopupOptions(*owner.window, {200.f, 150.f});
    options.dismissOnOutsideClick = false;

    auto popup = X11PopupWindow {options};

    check(popup.isUp());

    const auto origin = x11PopupRootOriginOf(probe.connection, owner.id());

    input.moveTo(origin + Point {40.f, 40.f});
    Threads::runEventLoopFor(Time::MS {50});

    input.click(1);

    Threads::runEventLoopUntil([&] { return owner.content.ups > 0; },
                               x11PopupTestTimeout);

    check(popup.dismissals == 0, "the popup dismissed itself on an outside press");
    check(owner.content.downs == 1, "the press never reached the view it landed on");
};

// The pointer belongs to the menu for exactly as long as the menu is up: a
// grab left behind would freeze every other client on the display.
auto tX11PopupReleasesThePointer = test("X11/popupReleasesThePointerWhenItGoes") = []
{
    if (!x11PopupServerReachable())
        return;

    auto grabber = X11PopupConnection {};

    if (!x11PopupArrived(grabber.isValid()))
        return;

    auto owner = X11PopupOwner {};

    {
        auto popup = X11PopupWindow {x11PopupOptions(*owner.window, {200.f, 150.f})};

        check(popup.isUp());
        check(!x11PopupPointerIsFree(grabber), "the popup never took the pointer");
    }

    Threads::runEventLoopFor(Time::MS {150});

    check(x11PopupPointerIsFree(grabber),
          "the popup kept the pointer after it was destroyed");
};

// The order an app hits on quit: the window the menu pops over is torn down
// with the menu still up.
auto tX11PopupSurvivesTheOwner = test("X11/popupSurvivesTheOwnerGoingFirst") = []
{
    if (!x11PopupServerReachable())
        return;

    auto popup = std::unique_ptr<X11PopupWindow> {};

    {
        auto owner = X11PopupOwner {};

        popup = std::make_unique<X11PopupWindow>(
            x11PopupOptions(*owner.window, {200.f, 150.f}));

        check(popup->isUp());
    }

    // The owner going away is the end of the menu, and the request has to
    // survive the window that provoked it going first.
    Threads::runEventLoopUntil([&] { return popup->dismissals > 0; },
                               x11PopupTestTimeout);

    check(popup->dismissals > 0,
          "the owner going away did not ask for the popup to be dismissed");

    popup.reset();
};

// A host hands over whatever window its editor sits in, which need not be the
// one a window manager knows: WM_TRANSIENT_FOR has to name the toplevel.
auto tX11PopupNativeParentResolves =
    test("X11/popupNativeParentResolvesToItsToplevel") = []
{
    if (!x11PopupServerReachable())
        return;

    auto probe = X11PopupConnection {};

    if (!x11PopupArrived(probe.isValid()))
        return;

    auto owner = X11PopupOwner {};

    // A reparenting window manager frames the toplevel, and the walk up then
    // stops at the frame rather than at the window this case knows about.
    if (x11PopupParentOf(probe.connection, owner.id()) != probe.root())
    {
        LOG("A window manager has reparented the owner, so the toplevel this "
            "case would assert against is its frame rather than the window it "
            "made. Scripts/with-xvfb runs it for real, which is what CI does.");
        return;
    }

    // The child a host would hand over: another client's window inside the
    // owner, which is exactly the shape of a plugin's editor.
    const auto child = xcb_generate_id(probe.connection);
    const uint32_t noAttributes[] = {0};

    xcb_create_window(probe.connection,
                      XCB_COPY_FROM_PARENT,
                      child,
                      owner.id(),
                      0,
                      0,
                      100,
                      80,
                      0,
                      XCB_WINDOW_CLASS_INPUT_OUTPUT,
                      XCB_COPY_FROM_PARENT,
                      0,
                      noAttributes);
    xcb_map_window(probe.connection, child);
    xcb_flush(probe.connection);

    auto options = WindowOptions {};
    options.popup = true;
    options.nativeParent = reinterpret_cast<void*>((uintptr_t) child);
    options.width = 160;
    options.height = 120;
    options.initialPosition = owner.window->getPosition() + Point {200.f, 150.f};

    {
        auto popup = X11PopupWindow {options};

        check(popup.isUp());

        const auto transient = x11PopupWordProperty(
            probe.connection, popup.id(), XCB_ATOM_WM_TRANSIENT_FOR);

        if (x11PopupArrived(transient.has_value(),
                            "the popup carries no WM_TRANSIENT_FOR"))
            check(*transient == owner.id(),
                  "a child window id did not resolve to its toplevel");
    }

    xcb_destroy_window(probe.connection, child);
    xcb_flush(probe.connection);
};

// The standing bug a popup's grab made visible, and the reason the tracker
// forgets a press when the pointer moves to another window of ours: a host
// left holding a button reports the next press inside the plugin as a drag,
// and the click after it as a double.
auto tX11PopupPointerTrackerResets =
    test("X11/popupPointerTrackerForgetsAHeldPress") = []
{
    auto tracker = PointerTracker {};

    tracker.setPosition({10.f, 10.f});

    check(tracker.pressed(MouseButton::Left, 1000) == 1);
    check(tracker.isButtonHeld());
    check(tracker.getDownPosition().x == 10.f);

    tracker.reset();

    check(!tracker.isButtonHeld(), "the button is still held after a reset");
    check(tracker.getClickCount() == 0);
    check(tracker.getDownPosition().x == 0.f);

    // The position is the pointer's and not the press's: it is wherever it is
    // whatever this tracker has forgotten.
    check(tracker.getPosition().x == 10.f);

    // And the next press is a first click, not the double the timestamp and
    // the position alone would have made it.
    check(tracker.pressed(MouseButton::Left, 1010) == 1,
          "a press after a reset was counted as a double click");
};
