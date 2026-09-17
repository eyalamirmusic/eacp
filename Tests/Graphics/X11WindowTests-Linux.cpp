#include "Common.h"

#include <eacp/Core/Threads/EventLoop.h>
#include <eacp/Core/Utils/Environment.h>
#include <eacp/Graphics/Graphics/Keyboard.h>
#include <eacp/Graphics/View/View-Linux.h>
#include <eacp/Graphics/Window/LinuxSeat-Linux.h>
#include <eacp/Graphics/Window/LinuxWindowSystem-Linux.h>
#include <eacp/Graphics/Window/X11Input-Linux.h>

#include <xcb/randr.h>
#include <xcb/xcb.h>
#include <xcb/xfixes.h>
#include <xcb/xinput.h>

#if EACP_HAS_XTEST
#include <xcb/xtest.h>
#endif

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <source_location>
#include <linux/input-event-codes.h>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

// The X11 backend against a real server: not headless, and every case
// self-skips without one, unless EACP_REQUIRE_DISPLAY=1. The input cases drive
// the server's own pointer and keyboard through XTest, so what they assert is
// the whole path from a real device event to a View; they skip again where the
// server has no XTest, and on Xwayland, whose seat belongs to the compositor
// rather than to the X server (serverOwnsItsSeat below).
//
// main() defaults EACP_WINDOW_SYSTEM to x11, so running this binary by hand on
// a Wayland desktop exercises the X11 backend under XWayland with no variable
// to remember; Scripts/with-xvfb runs the same cases with no window manager at
// all, which is the harsher target and the one the assertions are written
// against.

using namespace nano;
using namespace eacp;
using namespace eacp::Graphics;

namespace
{
constexpr auto x11TestTimeout = Time::MS {5000};

// Decided by trying: the backend degrades to a surfaceless window silently.
// The override main() defaults is checked too, so a run told to prefer Wayland
// skips rather than testing the wrong backend through this file's assertions.
bool x11ServerReachable()
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

// Only Scripts/with-xvfb starts a server this test may kill, and only there is
// a window's position the one it asked for.
bool onOwnXvfb()
{
    return !getEnvValue("EACP_XVFB_OUTPUT").empty();
}

xcb_window_t windowIdOf(Window& window)
{
    return (xcb_window_t) (uintptr_t) window.getHandle();
}

// A connection of the test's own, so nothing asserted here comes from the one
// the backend is being tested through.
struct TestConnection
{
    TestConnection()
        : connection(xcb_connect(nullptr, nullptr))
    {
        if (xcb_connection_has_error(connection) != 0)
        {
            xcb_disconnect(connection);
            connection = nullptr;
        }
    }

    ~TestConnection()
    {
        if (connection != nullptr)
            xcb_disconnect(connection);
    }

    bool isValid() const { return connection != nullptr; }

    xcb_connection_t* connection = nullptr;
};

struct TestGeometry
{
    bool found = false;
    int x = 0;
    int y = 0;
    int width = 0;
    int height = 0;
};

TestGeometry geometryOf(xcb_connection_t* connection, xcb_window_t window)
{
    auto geometry = TestGeometry {};

    if (connection == nullptr || window == XCB_NONE)
        return geometry;

    auto* reply = xcb_get_geometry_reply(
        connection, xcb_get_geometry(connection, window), nullptr);

    if (reply == nullptr)
        return geometry;

    geometry = {true, reply->x, reply->y, reply->width, reply->height};
    std::free(reply);

    return geometry;
}

// Where the window really is, which under a reparenting window manager is not
// what its own geometry says.
TestGeometry rootPositionOf(xcb_connection_t* connection, xcb_window_t window)
{
    auto position = geometryOf(connection, window);

    if (!position.found)
        return position;

    auto* setup = xcb_get_setup(connection);
    auto root = xcb_setup_roots_iterator(setup).data->root;

    auto* reply = xcb_translate_coordinates_reply(
        connection,
        xcb_translate_coordinates(connection, window, root, 0, 0),
        nullptr);

    if (reply == nullptr)
        return position;

    position.x = reply->dst_x;
    position.y = reply->dst_y;
    std::free(reply);

    return position;
}

// A content view that counts the one thing a scale change tells it.
struct ScaleCountingView : View
{
    void backingScaleChanged() override { ++scaleChanges; }

    int scaleChanges = 0;
};

// The view is declared first so it outlives the window.
struct ServerWindow
{
    explicit ServerWindow(int width = 640, int height = 400)
    {
        auto options = WindowOptions {};
        options.width = width;
        options.height = height;
        options.title = "eacp X11 tests";

        // A close here must not take the test runner down with it.
        options.isPrimary = false;

        window.emplace(options);

        window->events.onMoved = [this](Point position)
        {
            ++moves;
            lastPosition = position;
        };

        window->events.onHidden = [this] { ++hidden; };

        window->setContentView(content);

        Threads::runEventLoopUntil([this] { return window->isVisible(); },
                                   x11TestTimeout);
    }

    bool isUp() { return window.has_value() && window->isVisible(); }

    xcb_window_t id() { return windowIdOf(*window); }

    ScaleCountingView content;
    int moves = 0;
    int hidden = 0;
    Point lastPosition;
    std::optional<Window> window;
};

struct PresentingView : View
{
    PresentingView()
        : record(requestViewSurface(*this))
    {
        record.onAvailable = [this] { ++available; };
        record.onLost = [this] { ++lost; };
        record.onResized = [this] { ++resized; };
        record.onFrameDone = [this] { ++frames; };
    }

    ~PresentingView() override
    {
        // ~View fires onLost when this object's own members are already gone.
        record.onAvailable = [] {};
        record.onLost = [] {};
        record.onResized = [] {};
        record.onFrameDone = [] {};
    }

    bool hasSurface() const { return record.handle.isValid(); }

    xcb_window_t id() const { return (xcb_window_t) record.handle.window; }

    ViewSurface& record;
    int available = 0;
    int lost = 0;
    int resized = 0;
    int frames = 0;
};

// A view that remembers what it was told, and nothing else. Mouse events only
// reach a view that asks for them.
struct RecordingView : View
{
    RecordingView() { getProperties().handlesMouseEvents = true; }

    void mouseDown(const MouseEvent& event) override { downs.push_back(event); }
    void mouseUp(const MouseEvent& event) override { ups.push_back(event); }
    void mouseDragged(const MouseEvent& event) override { drags.push_back(event); }
    void mouseMoved(const MouseEvent& event) override { moves.push_back(event); }
    void mouseWheel(const MouseEvent& event) override { wheels.push_back(event); }
    void keyDown(const KeyEvent& event) override { keyDowns.push_back(event); }
    void keyUp(const KeyEvent& event) override { keyUps.push_back(event); }

    void forget()
    {
        downs.clear();
        ups.clear();
        drags.clear();
        moves.clear();
        wheels.clear();
        keyDowns.clear();
        keyUps.clear();
    }

    std::vector<MouseEvent> downs;
    std::vector<MouseEvent> ups;
    std::vector<MouseEvent> drags;
    std::vector<MouseEvent> moves;
    std::vector<MouseEvent> wheels;
    std::vector<KeyEvent> keyDowns;
    std::vector<KeyEvent> keyUps;
};

// A presenting view that also takes mouse events: its child window selects no
// input at all, so everything over it has to propagate to the toplevel and be
// hit-tested back down to here.
struct RecordingPresenter : RecordingView
{
    RecordingPresenter()
        : record(requestViewSurface(*this))
    {
        record.onAvailable = [this] { ++available; };
    }

    ~RecordingPresenter() override
    {
        record.onAvailable = [] {};
    }

    ViewSurface& record;
    int available = 0;
};

// Every other suite's windows come up at the origin, this file's own included,
// and a display has one stack: a window on top of this one would take the
// clicks meant for it. The far corner is where nothing else lands.
Point inputWindowOrigin(int width, int height)
{
    const auto display = primaryDisplay().frame;

    return {std::max(display.w - (float) width - 40.f, 0.f),
            std::max(display.h - (float) height - 40.f, 0.f)};
}

// The same window every input case works on: small, out of the way, and with a
// view that records.
struct InputWindow
{
    InputWindow(int width = 400, int height = 300)
    {
        auto options = WindowOptions {};
        options.width = width;
        options.height = height;
        options.title = "eacp X11 input tests";
        options.isPrimary = false;
        options.initialPosition = inputWindowOrigin(width, height);

        window.emplace(options);

        window->events.onActivationChanged = [this](bool nowActive)
        {
            active = nowActive;
            ++activations;
        };

        window->setContentView(content);

        Threads::runEventLoopUntil([this] { return window->isVisible(); },
                                   x11TestTimeout);
    }

    ~InputWindow()
    {
        if (window)
            window->setMouseLocked(false);
    }

    bool isUp() { return window.has_value() && window->isVisible(); }

    xcb_window_t id() { return windowIdOf(*window); }

    RecordingView content;
    bool active = false;
    int activations = 0;
    std::optional<Window> window;
};

// XTest against the test's own connection: the server's real pointer and
// keyboard, so what the backend sees is indistinguishable from a device.
struct FakeInput
{
    explicit FakeInput(xcb_connection_t* connectionToUse)
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
    // and does nothing with it - the pointer belongs to the compositor there -
    // while WarpPointer moves it on both servers and generates the same
    // MotionNotify a hand would. It is what xdotool does for the same reason.
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

    // XTest's own motion, which is relative when the detail is non-zero: the
    // server's virtual XTEST pointer posts it as a device would, so it is the
    // only thing here that makes an XI2 raw event - a warp makes none, which
    // is the whole reason a locked pointer is measured from one.
    void moveBy(Point delta)
    {
        fake(XCB_MOTION_NOTIFY, 1, (int16_t) delta.x, (int16_t) delta.y);
    }

    void button(uint8_t number, bool pressed)
    {
        fake(pressed ? XCB_BUTTON_PRESS : XCB_BUTTON_RELEASE, number, 0, 0);
    }

    void click(uint8_t number)
    {
        button(number, true);
        button(number, false);
    }

    // The evdev code the framework speaks, turned back into what the server
    // wants: X11 keycodes are evdev + 8.
    void key(uint32_t evdevCode, bool pressed)
    {
        fake(pressed ? XCB_KEY_PRESS : XCB_KEY_RELEASE,
             (uint8_t) (evdevCode + 8),
             0,
             0);
    }

    void fake(uint8_t type, uint8_t detail, int16_t x, int16_t y)
    {
#if EACP_HAS_XTEST
        if (!available)
            return;

        xcb_test_fake_input(
            connection, type, detail, XCB_CURRENT_TIME, root, x, y, 0);
        xcb_flush(connection);
#else
        (void) type;
        (void) detail;
        (void) x;
        (void) y;
#endif
    }

    bool available = false;
    xcb_window_t root = XCB_NONE;
    xcb_connection_t* connection = nullptr;
};

// Where the window's top left really is, whatever a window manager's frame put
// around it, so a local point can be aimed at in root coordinates.
Point rootOriginOf(xcb_connection_t* probe, xcb_window_t window)
{
    const auto position = rootPositionOf(probe, window);

    return {(float) position.x, (float) position.y};
}

// check() reports and carries on, which is what a suite wants - but a case
// that then reads the event it has just failed to find would take the process
// down with it instead of failing.
bool checkArrived(
    bool condition,
    std::string_view message = {},
    const std::source_location& location = std::source_location::current())
{
    check(condition, message, location);

    return condition;
}

// Under Xwayland the pointer and the keyboard belong to the compositor: an X
// client's synthetic input moves the server's own idea of where the pointer is
// without the compositor ever hearing about it, so nothing is delivered to a
// window that has not grabbed - the same shape of problem as Weston's headless
// backend advertising no seat. The extension is on no other server, which
// makes it the test.
bool serverOwnsItsSeat(xcb_connection_t* probe)
{
    auto* reply = xcb_query_extension_reply(
        probe, xcb_query_extension(probe, 8, "XWAYLAND"), nullptr);

    const auto compositorOwned = reply != nullptr && reply->present != 0;
    std::free(reply);

    return !compositorOwned;
}

// Nothing to assert without a server whose seat is its own to drive; a case
// that cannot run says so once rather than failing.
bool canDriveTheSeat(xcb_connection_t* probe, const FakeInput& input)
{
    if (!input.available)
    {
        LOG("XTest is not available on this server: the input cases cannot "
            "synthesise a pointer or a key, and are skipped.");

        return false;
    }

    if (!serverOwnsItsSeat(probe))
    {
        LOG("This is an Xwayland server, whose pointer and keyboard the "
            "compositor owns: neither XTest nor a warp reaches a window that "
            "has not grabbed, so the input cases are skipped. Scripts/"
            "with-xvfb runs them for real, which is what CI does.");

        return false;
    }

    return true;
}

xcb_atom_t atomNamed(xcb_connection_t* probe, const char* name)
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

// Nothing else on this display may be over the window a case is aiming at, and
// there are two ways to say so: with no window manager the stack is the
// client's own to change, and with one the request that counts is
// _NET_WM_STATE_ABOVE. Both are sent, because both servers this suite runs on
// are real - and a desktop session's window manager maps a window from an
// application that is not the focused one behind whatever is.
void raiseWindow(xcb_connection_t* probe, xcb_window_t window)
{
    const uint32_t above = XCB_STACK_MODE_ABOVE;

    xcb_configure_window(probe, window, XCB_CONFIG_WINDOW_STACK_MODE, &above);

    const auto root = xcb_setup_roots_iterator(xcb_get_setup(probe)).data->root;

    auto event = xcb_client_message_event_t {};
    event.response_type = XCB_CLIENT_MESSAGE;
    event.format = 32;
    event.window = window;
    event.type = atomNamed(probe, "_NET_WM_STATE");

    // Add the state, and say it comes from an ordinary application.
    event.data.data32[0] = 1;
    event.data.data32[1] = atomNamed(probe, "_NET_WM_STATE_ABOVE");
    event.data.data32[3] = 1;

    xcb_send_event(probe,
                   0,
                   root,
                   XCB_EVENT_MASK_SUBSTRUCTURE_NOTIFY
                       | XCB_EVENT_MASK_SUBSTRUCTURE_REDIRECT,
                   reinterpret_cast<const char*>(&event));
    xcb_flush(probe);

    // A window manager restacks on its own time, and the pointer must not be
    // aimed at the window before it has.
    Threads::runEventLoopFor(Time::MS {150});
}

// What a case that is not about focus wants: the keyboard, with no click to
// stack against and no window manager policy in the way.
bool takeFocus(xcb_connection_t* probe, InputWindow& host)
{
    raiseWindow(probe, host.id());

    xcb_set_input_focus(probe, XCB_INPUT_FOCUS_PARENT, host.id(), XCB_CURRENT_TIME);
    xcb_flush(probe);

    Threads::runEventLoopUntil([&] { return host.active; }, x11TestTimeout);

    host.content.forget();

    return host.active;
}

// Whether XI2 is carrying this window's pointer events. all_event_masks is the
// union of what every client selected on the window, and nothing but the
// backend has ours selected: with XI2 there the core pointer bits are simply
// not among them, because a window that selected an XI2 event and the core one
// it replaces would be told about the same press twice on some servers.
bool pointerGoesThroughXinput(xcb_connection_t* probe, xcb_window_t window)
{
    auto* reply = xcb_get_window_attributes_reply(
        probe, xcb_get_window_attributes(probe, window), nullptr);

    if (reply == nullptr)
        return false;

    const auto core = (reply->all_event_masks & XCB_EVENT_MASK_POINTER_MOTION) != 0;

    std::free(reply);

    return !core;
}

// Where the server has the pointer right now, in root coordinates.
Point pointerPosition(xcb_connection_t* probe, xcb_window_t root)
{
    auto* reply =
        xcb_query_pointer_reply(probe, xcb_query_pointer(probe, root), nullptr);

    if (reply == nullptr)
        return {};

    const auto position = Point {(float) reply->root_x, (float) reply->root_y};
    std::free(reply);

    return position;
}

// Set by Scripts/with-xvfb, and by nothing a real desktop session runs.
std::optional<Point> xvfbOutputSize()
{
    const auto value = getEnvValue("EACP_XVFB_OUTPUT");
    const auto separator = value.find('x');

    if (separator == std::string::npos)
        return {};

    return Point {std::stof(value.substr(0, separator)),
                  std::stof(value.substr(separator + 1))};
}

// A geometry the server reports is in pixels and everything a case asks a
// window for is in points: the same number only where the display's scale is
// 1, which is every server this suite starts for itself and not a desktop
// whose session has written an Xft.dpi.
int pixelsOf(float points)
{
    return (int) std::lround(points * primaryDisplay().backingScale);
}

float pointsOf(int pixels)
{
    return (float) pixels / primaryDisplay().backingScale;
}

// The xrdb database is one STRING property on the root that the whole display
// shares: a session's settings daemon owns it, and only a server this suite
// started for itself may have it written to. A desktop's own scale and cursor
// theme are not this suite's to change, so the cases that write it run where
// Scripts/with-xvfb is the one that started the server.
bool canChangeTheResourceDatabase()
{
    if (onOwnXvfb())
        return true;

    LOG("The resource database on this display belongs to the desktop session "
        "rather than to this suite, so the cases that rewrite Xft.dpi and the "
        "cursor theme are skipped. Scripts/with-xvfb runs them for real, which "
        "is what CI does.");

    return false;
}

std::string resourceDatabaseOf(xcb_connection_t* probe)
{
    const auto root = xcb_setup_roots_iterator(xcb_get_setup(probe)).data->root;

    auto* reply = xcb_get_property_reply(
        probe,
        xcb_get_property(
            probe, 0, root, XCB_ATOM_RESOURCE_MANAGER, XCB_ATOM_STRING, 0, 65536),
        nullptr);

    if (reply == nullptr)
        return {};

    auto database = std::string {(const char*) xcb_get_property_value(reply),
                                 (size_t) xcb_get_property_value_length(reply)};

    std::free(reply);

    return database;
}

// What the backend made of the database, seen through the one public thing
// that reports it.
bool scaleBecomes(float wanted)
{
    Threads::runEventLoopUntil([wanted]
                               { return primaryDisplay().backingScale == wanted; },
                               x11TestTimeout);

    return primaryDisplay().backingScale == wanted;
}

// Writes the database and puts back whatever was there, waiting each time for
// the backend to have taken it: a case that left a scale of 2 behind would
// hand it to the next case in a direct run, where the whole file is one
// process.
struct ResourceDatabase
{
    explicit ResourceDatabase(xcb_connection_t* probeToUse)
        : probe(probeToUse)
        , original(resourceDatabaseOf(probeToUse))
        , originalScale(primaryDisplay().backingScale)
    {
    }

    ~ResourceDatabase()
    {
        write(original);
        scaleBecomes(originalScale);
    }

    void write(const std::string& database)
    {
        const auto root = xcb_setup_roots_iterator(xcb_get_setup(probe)).data->root;

        xcb_change_property(probe,
                            XCB_PROP_MODE_REPLACE,
                            root,
                            XCB_ATOM_RESOURCE_MANAGER,
                            XCB_ATOM_STRING,
                            8,
                            (uint32_t) database.size(),
                            database.c_str());
        xcb_flush(probe);
    }

    xcb_connection_t* probe = nullptr;
    std::string original;
    float originalScale = 1.f;
};

// The size of the cursor the server is showing, which is the size the theme
// gave it. Zero where XFixes cannot answer.
Point cursorImageSize(xcb_connection_t* probe)
{
    auto* version = xcb_xfixes_query_version_reply(
        probe,
        xcb_xfixes_query_version(
            probe, XCB_XFIXES_MAJOR_VERSION, XCB_XFIXES_MINOR_VERSION),
        nullptr);

    if (version == nullptr)
        return {};

    std::free(version);

    auto* image = xcb_xfixes_get_cursor_image_reply(
        probe, xcb_xfixes_get_cursor_image(probe), nullptr);

    if (image == nullptr)
        return {};

    const auto size = Point {(float) image->width, (float) image->height};
    std::free(image);

    return size;
}

// Whether any theme with cursors in it is installed, which is what decides
// whether a size in the database can change anything: with none, xcb-cursor
// falls back to the server's own cursor font, whose glyphs are one size
// whatever the database says.
bool aCursorThemeIsInstalled()
{
    auto roots = std::vector<std::string> {};

    if (const auto path = getEnvValue("XCURSOR_PATH"); !path.empty())
    {
        for (auto rest = std::string_view {path}; !rest.empty();)
        {
            const auto end = rest.find(':');
            roots.emplace_back(rest.substr(0, end));
            rest = end == std::string_view::npos ? std::string_view {}
                                                 : rest.substr(end + 1);
        }
    }
    else
    {
        const auto home = getEnvValue("HOME");

        roots = {home + "/.local/share/icons",
                 home + "/.icons",
                 "/usr/share/icons",
                 "/usr/share/pixmaps"};
    }

    auto ignored = std::error_code {};

    for (const auto& root: roots)
        for (const auto& theme: std::filesystem::directory_iterator {root, ignored})
            if (std::filesystem::exists(theme.path() / "cursors", ignored))
                return true;

    return false;
}

// The output the server has now, read through the test's own connection: the
// root's size where RandR names no primary output, which is the case on Xvfb.
Point serverOutputSize(xcb_connection_t* probe)
{
    const auto root = xcb_setup_roots_iterator(xcb_get_setup(probe)).data->root;
    const auto geometry = geometryOf(probe, root);

    return {(float) geometry.width, (float) geometry.height};
}

bool randrIsUsable(xcb_connection_t* probe)
{
    auto* reply = xcb_randr_query_version_reply(
        probe, xcb_randr_query_version(probe, 1, 5), nullptr);

    if (reply == nullptr)
        return false;

    const auto usable = reply->major_version > 1 || reply->minor_version >= 3;
    std::free(reply);

    return usable;
}

// True when the server took the new screen size. Xvfb refuses every one of
// them: its screen's maximum is the size it was started at and its one crtc
// already covers all of it, so there is nothing to grow into and nothing the
// crtc would fit inside.
bool resizeTheScreen(xcb_connection_t* probe, Point size)
{
    constexpr auto millimetresPerPoint = 25.4f / 96.f;

    const auto root = xcb_setup_roots_iterator(xcb_get_setup(probe)).data->root;

    auto* error = xcb_request_check(probe,
                                    xcb_randr_set_screen_size_checked(
                                        probe,
                                        root,
                                        (uint16_t) size.x,
                                        (uint16_t) size.y,
                                        (uint32_t) (size.x * millimetresPerPoint),
                                        (uint32_t) (size.y * millimetresPerPoint)));

    if (error == nullptr)
        return true;

    std::free(error);

    return false;
}

xcb_randr_output_t primaryOutputOf(xcb_connection_t* probe)
{
    const auto root = xcb_setup_roots_iterator(xcb_get_setup(probe)).data->root;

    auto* reply = xcb_randr_get_output_primary_reply(
        probe, xcb_randr_get_output_primary(probe, root), nullptr);

    if (reply == nullptr)
        return XCB_NONE;

    const auto output = reply->output;
    std::free(reply);

    return output;
}

xcb_randr_output_t firstOutputOf(xcb_connection_t* probe)
{
    const auto root = xcb_setup_roots_iterator(xcb_get_setup(probe)).data->root;

    auto* reply = xcb_randr_get_screen_resources_current_reply(
        probe, xcb_randr_get_screen_resources_current(probe, root), nullptr);

    if (reply == nullptr)
        return XCB_NONE;

    const auto* outputs = xcb_randr_get_screen_resources_current_outputs(reply);
    const auto count = xcb_randr_get_screen_resources_current_outputs_length(reply);

    const auto output = count > 0 ? outputs[0] : (xcb_randr_output_t) XCB_NONE;
    std::free(reply);

    return output;
}

void setPrimaryOutput(xcb_connection_t* probe, xcb_randr_output_t output)
{
    const auto root = xcb_setup_roots_iterator(xcb_get_setup(probe)).data->root;

    xcb_randr_set_output_primary(probe, root, output);
    xcb_flush(probe);
}
} // namespace

// The one case that does not self-skip.
auto tServerIsPresentWhenRequired = test("X11/aServerIsPresentWhenRequired") = []
{
    const auto reachable = x11ServerReachable();

    LOG("X server: ",
        reachable ? getEnvValue("DISPLAY") : std::string {"none reached"});

    if (getEnvValue("EACP_REQUIRE_DISPLAY") != "1")
        return;

    check(reachable,
          "EACP_REQUIRE_DISPLAY=1 but no X server was reached - every other "
          "case in this binary would have skipped and reported a pass");
};

auto tWindowComesUpAtItsSize = test("X11/windowComesUpAtItsConfiguredSize") = []
{
    if (!x11ServerReachable())
        return;

    auto host = ServerWindow {640, 400};

    check(host.isUp());

    const auto bounds = host.content.getBounds();

    check(bounds.w == 640.f);
    check(bounds.h == 400.f);
};

auto tHandleIsTheWindowId = test("X11/handleIsTheServersWindowId") = []
{
    if (!x11ServerReachable())
        return;

    auto host = ServerWindow {640, 400};
    auto probe = TestConnection {};

    check(probe.isValid());
    check(host.id() != XCB_NONE);

    const auto geometry = geometryOf(probe.connection, host.id());

    check(geometry.found, "the id the handle carries is not a window");
    check(geometry.width == pixelsOf(640.f));
    check(geometry.height == pixelsOf(400.f));

    check(host.window->getContentViewHandle() != host.window->getHandle());
};

auto tHidingUnmapsTheWindow = test("X11/hidingUnmapsAndShowingMapsAgain") = []
{
    if (!x11ServerReachable())
        return;

    auto host = ServerWindow {400, 300};

    check(host.isUp());

    host.window->setVisible(false);

    check(!host.window->isVisible());

    host.window->setVisible(true);

    Threads::runEventLoopUntil([&] { return host.window->isVisible(); },
                               x11TestTimeout);

    check(host.window->isVisible(), "the window never mapped again");
};

auto tSetPositionMovesTheWindow =
    test("X11/setPositionMovesTheWindowAndReports") = []
{
    if (!x11ServerReachable())
        return;

    auto host = ServerWindow {320, 240};
    auto probe = TestConnection {};

    check(probe.isValid());

    const auto wanted = Point {220.f, 160.f};

    // Whatever a window manager did while the window was coming up is not this
    // move.
    host.moves = 0;

    host.window->setPosition(wanted);

    // The value put in is the one that comes back out, at once and once only:
    // an app saving its window position from onMoved reads it back straight
    // away, wherever the request eventually lands it.
    check(host.moves == 1, "onMoved did not fire for a programmatic move");
    check(host.lastPosition.x == wanted.x);
    check(host.lastPosition.y == wanted.y);
    check(host.window->getPosition().x == wanted.x);

    // And the ConfigureNotify corrects it where a window manager's frame put
    // the window somewhere other than where it was asked to.
    Threads::runEventLoopUntil(
        [&]
        {
            const auto placed = rootPositionOf(probe.connection, host.id());

            return placed.found && host.lastPosition.x == pointsOf(placed.x)
                   && host.lastPosition.y == pointsOf(placed.y);
        },
        x11TestTimeout);

    const auto position = rootPositionOf(probe.connection, host.id());

    check(position.found);

    // What the server put it at is what was reported, whatever a window
    // manager's frame did to the request.
    check(host.lastPosition.x == pointsOf(position.x));
    check(host.lastPosition.y == pointsOf(position.y));
    check(host.window->getPosition().x == pointsOf(position.x));

    // With no window manager in the way the request is honoured exactly.
    if (onOwnXvfb())
    {
        check(host.lastPosition.x == wanted.x);
        check(host.lastPosition.y == wanted.y);
    }
};

auto tOutsideResizeReachesTheContentView =
    test("X11/aResizeFromOutsideResizesTheContentView") = []
{
    if (!x11ServerReachable())
        return;

    // A second client's ConfigureWindow on a toplevel is a ConfigureRequest
    // wherever a window manager holds SubstructureRedirect, and granting it is
    // the window manager's to decide: Mutter passes this one through, i3
    // enforces its own layout instead. Only the Xvfb this suite starts itself
    // has nobody to ask.
    if (!onOwnXvfb())
        return;

    auto resizes = 0;
    auto content = View {};

    auto options = WindowOptions {};
    options.width = 640;
    options.height = 400;
    options.isPrimary = false;
    options.onResize = [&resizes](int, int) { ++resizes; };

    auto window = std::optional<Window> {};
    window.emplace(options);
    window->setContentView(content);

    Threads::runEventLoopUntil([&] { return window->isVisible(); }, x11TestTimeout);
    check(window->isVisible());

    auto probe = TestConnection {};
    check(probe.isValid());

    const uint32_t size[] = {500, 320};

    xcb_configure_window(probe.connection,
                         windowIdOf(*window),
                         XCB_CONFIG_WINDOW_WIDTH | XCB_CONFIG_WINDOW_HEIGHT,
                         size);
    xcb_flush(probe.connection);

    Threads::runEventLoopUntil([&] { return content.getBounds().w == 500.f; },
                               x11TestTimeout);

    check(content.getBounds().w == 500.f, "the content view never resized");
    check(content.getBounds().h == 320.f);
    check(resizes > 0, "onResize never fired");
};

auto tDeleteWindowHidesAHidesOnCloseWindow =
    test("X11/wmDeleteWindowHidesAHidesOnCloseWindow") = []
{
    if (!x11ServerReachable())
        return;

    auto content = View {};

    auto options = WindowOptions {};
    options.isPrimary = false;
    options.hidesOnClose = true;

    auto window = std::optional<Window> {};
    window.emplace(options);

    auto hidden = 0;
    window->events.onHidden = [&hidden] { ++hidden; };

    window->setContentView(content);

    Threads::runEventLoopUntil([&] { return window->isVisible(); }, x11TestTimeout);
    check(window->isVisible());

    auto probe = TestConnection {};
    check(probe.isValid());

    auto protocols = xcb_intern_atom_reply(
        probe.connection,
        xcb_intern_atom(probe.connection, 0, 12, "WM_PROTOCOLS"),
        nullptr);
    auto deleteWindow = xcb_intern_atom_reply(
        probe.connection,
        xcb_intern_atom(probe.connection, 0, 16, "WM_DELETE_WINDOW"),
        nullptr);

    check(protocols != nullptr && deleteWindow != nullptr);

    auto event = xcb_client_message_event_t {};
    event.response_type = XCB_CLIENT_MESSAGE;
    event.format = 32;
    event.window = windowIdOf(*window);
    event.type = protocols->atom;
    event.data.data32[0] = deleteWindow->atom;

    std::free(protocols);
    std::free(deleteWindow);

    xcb_send_event(probe.connection,
                   0,
                   event.window,
                   XCB_EVENT_MASK_NO_EVENT,
                   reinterpret_cast<const char*>(&event));
    xcb_flush(probe.connection);

    Threads::runEventLoopUntil([&] { return hidden > 0; }, x11TestTimeout);

    check(hidden == 1, "the close request never reached the window");
    check(!window->isVisible());
};

auto tViewSurfaceIsAChildWindow = test("X11/viewSurfaceIsAChildWindow") = []
{
    if (!x11ServerReachable())
        return;

    auto host = ServerWindow {640, 400};
    auto presenter = PresentingView {};

    presenter.setBounds({10.f, 20.f, 320.f, 240.f});
    host.content.addSubview(presenter);

    Threads::runEventLoopUntil([&] { return presenter.available > 0; },
                               x11TestTimeout);

    check(presenter.available == 1);
    check(presenter.lost == 0);
    check(presenter.record.handle.kind == NativeSurfaceHandle::Kind::X11);
    check(presenter.record.handle.connection != nullptr);
    check(presenter.id() != XCB_NONE);

    check(presenter.record.scale == primaryDisplay().backingScale);
    check(presenter.record.pixelWidth == pixelsOf(320.f));
    check(presenter.record.pixelHeight == pixelsOf(240.f));

    auto probe = TestConnection {};
    check(probe.isValid());

    const auto geometry = geometryOf(probe.connection, presenter.id());

    check(geometry.found, "the record names no window the server knows");
    check(geometry.x == pixelsOf(10.f));
    check(geometry.y == pixelsOf(20.f));
    check(geometry.width == pixelsOf(320.f));
    check(geometry.height == pixelsOf(240.f));
};

auto tMovingTheViewMovesItsChild = test("X11/movingTheViewMovesItsChildWindow") = []
{
    if (!x11ServerReachable())
        return;

    auto host = ServerWindow {640, 400};
    auto presenter = PresentingView {};

    presenter.setBounds({0.f, 0.f, 320.f, 240.f});
    host.content.addSubview(presenter);

    Threads::runEventLoopUntil([&] { return presenter.available > 0; },
                               x11TestTimeout);
    check(presenter.available == 1);

    const auto resizesBefore = presenter.resized;

    presenter.setBounds({40.f, 60.f, 160.f, 120.f});

    check(presenter.resized == resizesBefore + 1);
    check(presenter.record.pixelWidth == pixelsOf(160.f));
    check(presenter.record.pixelHeight == pixelsOf(120.f));

    auto probe = TestConnection {};
    check(probe.isValid());

    const auto geometry = geometryOf(probe.connection, presenter.id());

    check(geometry.found);
    check(geometry.x == pixelsOf(40.f));
    check(geometry.y == pixelsOf(60.f));
    check(geometry.width == pixelsOf(160.f));
    check(geometry.height == pixelsOf(120.f));

    // A move with no size change is not a resize.
    const auto resizesAfterMove = presenter.resized;
    presenter.setBounds({80.f, 90.f, 160.f, 120.f});
    check(presenter.resized == resizesAfterMove);
};

auto tRemovingTheViewDestroysTheChild =
    test("X11/removingTheViewDestroysItsChildWindow") = []
{
    if (!x11ServerReachable())
        return;

    auto host = ServerWindow {640, 400};
    auto presenter = PresentingView {};

    presenter.setBounds({0.f, 0.f, 200.f, 100.f});
    host.content.addSubview(presenter);

    Threads::runEventLoopUntil([&] { return presenter.available > 0; },
                               x11TestTimeout);
    check(presenter.available == 1);

    const auto child = presenter.id();

    host.content.removeSubview(presenter);

    check(presenter.lost == 1);
    check(!presenter.hasSurface());
    check(presenter.record.pixelWidth == 0);
    check(!presenter.record.frameCallbackPending);

    auto probe = TestConnection {};
    check(probe.isValid());

    // The destroy is behind a flush on the backend's connection, so the probe
    // only sees it once the server has run the request.
    check(!geometryOf(probe.connection, child).found,
          "the child window outlived the view that owned it");
};

// X11 has no wl_surface.frame: this is the pacer of plan.md D7 answering.
auto tFrameCallbackArrives = test("X11/frameCallbackArrivesFromThePacer") = []
{
    if (!x11ServerReachable())
        return;

    auto host = ServerWindow {640, 400};
    auto presenter = PresentingView {};

    presenter.setBounds({0.f, 0.f, 320.f, 240.f});
    host.content.addSubview(presenter);

    Threads::runEventLoopUntil([&] { return presenter.available > 0; },
                               x11TestTimeout);
    check(presenter.available == 1);
    check(presenter.frames == 0, "a frame was answered before one was asked for");

    presenter.record.requestFrameCallback();
    check(presenter.record.frameCallbackPending);

    Threads::runEventLoopUntil([&] { return presenter.frames > 0; }, x11TestTimeout);

    check(presenter.frames == 1);
    check(!presenter.record.frameCallbackPending);

    // And the pacer answers again, rather than only ever once.
    presenter.record.requestFrameCallback();

    Threads::runEventLoopUntil([&] { return presenter.frames > 1; }, x11TestTimeout);

    check(presenter.frames == 2);
};

auto tPrimaryDisplayReportsTheOutput =
    test("X11/primaryDisplayReportsTheOutput") = []
{
    if (!x11ServerReachable())
        return;

    const auto display = primaryDisplay();

    check(display.frame.w > 0.f);
    check(display.frame.h > 0.f);

    // X11 has one scale for the whole display, and it is Xft.dpi over 96: a
    // desktop that names none is at 1, and Xvfb never names one.
    check(display.backingScale > 0.f);

    // No work area is published, so it is the whole display.
    check(display.workArea.w == display.frame.w);
    check(display.workArea.h == display.frame.h);

    // Only Scripts/with-xvfb knows the output size in advance - in pixels,
    // where a Display is in points.
    if (const auto expected = xvfbOutputSize())
    {
        check(display.backingScale == 1.f, "Xvfb has no resource database");
        check(display.frame.w == expected->x);
        check(display.frame.h == expected->y);
    }
};

auto tXftDpiIsTheScaleOfANewToplevel =
    test("X11/xftDpiIsTheScaleANewToplevelComesUpAt") = []
{
    if (!x11ServerReachable() || !canChangeTheResourceDatabase())
        return;

    auto probe = TestConnection {};

    if (!checkArrived(probe.isValid()))
        return;

    auto database = ResourceDatabase {probe.connection};

    database.write("Xft.dpi:\t192\n");

    if (!checkArrived(scaleBecomes(2.f), "the backend never read the new Xft.dpi"))
        return;

    // A Display is in points, so twice the scale is half the display.
    if (const auto expected = xvfbOutputSize())
    {
        check(primaryDisplay().frame.w == expected->x / 2.f);
        check(primaryDisplay().frame.h == expected->y / 2.f);
    }

    auto host = ServerWindow {320, 200};

    check(host.isUp());

    // The content is laid out in the points it was asked for...
    const auto bounds = host.content.getBounds();

    check(bounds.w == 320.f);
    check(bounds.h == 200.f);

    // ...and the window covers two pixels for each of them.
    const auto geometry = geometryOf(probe.connection, host.id());

    if (!checkArrived(geometry.found))
        return;

    check(geometry.width == 640, "the window did not take two pixels per point");
    check(geometry.height == 400);
};

// GTK rounds a fractional dpi to a whole factor and Qt does not; this backend
// does not either, so a desktop at 150% is 1.5 and every pixel derived from it
// is rounded once, where it is derived.
auto tAFractionalDpiIsNotRounded = test("X11/aFractionalXftDpiIsNotRounded") = []
{
    if (!x11ServerReachable() || !canChangeTheResourceDatabase())
        return;

    auto probe = TestConnection {};

    if (!checkArrived(probe.isValid()))
        return;

    auto database = ResourceDatabase {probe.connection};

    database.write("Xft.dpi:\t144\n");

    if (!checkArrived(scaleBecomes(1.5f), "144 dpi was not read as a scale of 1.5"))
        return;

    auto host = ServerWindow {320, 200};
    auto presenter = PresentingView {};

    check(host.isUp());

    presenter.setBounds({10.f, 20.f, 100.f, 60.f});
    host.content.addSubview(presenter);

    Threads::runEventLoopUntil([&] { return presenter.available > 0; },
                               x11TestTimeout);

    check(host.content.getBounds().w == 320.f);
    check(host.content.getBounds().h == 200.f);

    const auto geometry = geometryOf(probe.connection, host.id());

    if (!checkArrived(geometry.found))
        return;

    check(geometry.width == 480, "a window at 1.5 was not 480 pixels wide");
    check(geometry.height == 300);

    // And a presenting view inside it is the same arithmetic, rounded where
    // its own bounds are turned into pixels rather than beforehand.
    if (!checkArrived(presenter.available == 1))
        return;

    check(presenter.record.scale == 1.5f);
    check(presenter.record.pixelWidth == 150);
    check(presenter.record.pixelHeight == 90);

    const auto child = geometryOf(probe.connection, presenter.id());

    check(child.found);
    check(child.x == 15);
    check(child.y == 30);
    check(child.width == 150);
    check(child.height == 90);
};

auto tAResourceChangeRescalesAnOpenWindow =
    test("X11/aResourceManagerChangeRescalesAnOpenWindow") = []
{
    if (!x11ServerReachable() || !canChangeTheResourceDatabase())
        return;

    auto probe = TestConnection {};

    if (!checkArrived(probe.isValid()))
        return;

    auto database = ResourceDatabase {probe.connection};

    check(primaryDisplay().backingScale == 1.f, "Xvfb has no resource database");

    auto host = ServerWindow {320, 200};

    check(host.isUp());
    check(host.content.scaleChanges == 0);

    database.write("Xft.dpi:\t192\n");

    Threads::runEventLoopUntil([&] { return host.content.scaleChanges > 0; },
                               x11TestTimeout);

    if (!checkArrived(host.content.scaleChanges == 1,
                      "backingScaleChanged never reached the content view"))
        return;

    check(primaryDisplay().backingScale == 2.f, "Display kept the old scale");

    // The content keeps the point size it was laid out at, exactly as a
    // Wayland toplevel keeps its logical size; the pixels are what move.
    check(host.content.getBounds().w == 320.f);
    check(host.content.getBounds().h == 200.f);

    Threads::runEventLoopUntil(
        [&] { return geometryOf(probe.connection, host.id()).width == 640; },
        x11TestTimeout);

    const auto geometry = geometryOf(probe.connection, host.id());

    check(geometry.width == 640, "the window was never asked to grow");
    check(geometry.height == 400);

    // And back, so the window a case leaves behind is the one it started with.
    database.write(database.original);

    if (!checkArrived(scaleBecomes(1.f)))
        return;

    check(host.content.scaleChanges == 2);

    Threads::runEventLoopUntil(
        [&] { return geometryOf(probe.connection, host.id()).width == 320; },
        x11TestTimeout);

    check(geometryOf(probe.connection, host.id()).width == 320);
};

auto tPointerPositionsAreInPoints =
    test("X11/pointerPositionsArriveInPointsUnderAScale") = []
{
    if (!x11ServerReachable() || !canChangeTheResourceDatabase())
        return;

    auto probe = TestConnection {};

    if (!checkArrived(probe.isValid()))
        return;

    auto input = FakeInput {probe.connection};

    if (!canDriveTheSeat(probe.connection, input))
        return;

    auto database = ResourceDatabase {probe.connection};

    database.write("Xft.dpi:\t192\n");

    if (!checkArrived(scaleBecomes(2.f)))
        return;

    auto host = InputWindow {200, 150};

    check(host.isUp());

    raiseWindow(probe.connection, host.id());

    const auto origin = rootOriginOf(probe.connection, host.id());

    // The server measures the pointer in the window's pixels, and a view is
    // laid out in its points: half as many of them.
    input.moveTo(origin + Point {260.f, 180.f});

    Threads::runEventLoopUntil([&] { return !host.content.moves.empty(); },
                               x11TestTimeout);

    if (!checkArrived(!host.content.moves.empty(),
                      "no mouseMoved reached the content view"))
        return;

    const auto& moved = host.content.moves.back();

    check(moved.pos.x == 130.f, "the pointer arrived in pixels rather than points");
    check(moved.pos.y == 90.f);
};

// Xvfb's RandR is as fixed as a screen gets: one output, one mode, and a
// maximum screen size equal to the size it was started at, so RRSetScreenSize
// is refused and the frame cannot be made to move there. What every server
// does answer is the event - a primary-output change is an RRNotify - and what
// this asserts either way is that the output the backend reports is the one
// the server has now rather than the one it cached when it was first asked.
auto tARandrChangeRefreshesTheOutput =
    test("X11/aRandrChangeRefreshesThePrimaryOutput") = []
{
    if (!x11ServerReachable() || !onOwnXvfb())
        return;

    auto probe = TestConnection {};

    if (!checkArrived(probe.isValid()))
        return;

    if (!randrIsUsable(probe.connection))
    {
        LOG("This server has no RandR 1.3, so there is no output change to "
            "drive; the case is skipped.");
        return;
    }

    const auto scale = primaryDisplay().backingScale;

    // Asked for once, so what comes back afterwards is either a re-read or a
    // cache.
    check(primaryDisplay().frame.w == serverOutputSize(probe.connection).x / scale);

    const auto original = serverOutputSize(probe.connection);
    const auto halved = Point {original.x / 2.f, original.y / 2.f};

    if (resizeTheScreen(probe.connection, halved))
    {
        Threads::runEventLoopUntil(
            [&] { return primaryDisplay().frame.w == halved.x / scale; },
            x11TestTimeout);

        check(primaryDisplay().frame.w == halved.x / scale,
              "the display kept the size it had cached");

        check(resizeTheScreen(probe.connection, original));

        Threads::runEventLoopUntil(
            [&] { return primaryDisplay().frame.w == original.x / scale; },
            x11TestTimeout);

        check(primaryDisplay().frame.w == original.x / scale);

        return;
    }

    LOG("This server will not resize its screen - Xvfb never does - so the "
        "output change this drives is the primary output rather than the "
        "frame.");

    const auto primary = primaryOutputOf(probe.connection);
    const auto output = firstOutputOf(probe.connection);

    if (output == XCB_NONE)
    {
        LOG("This server has no RandR output at all; the case is skipped.");
        return;
    }

    setPrimaryOutput(probe.connection, output);

    Threads::runEventLoopFor(Time::MS {200});

    check(primaryDisplay().frame.w == serverOutputSize(probe.connection).x / scale,
          "the display no longer reports the output the server has");
    check(primaryDisplay().frame.h == serverOutputSize(probe.connection).y / scale);

    setPrimaryOutput(probe.connection, primary);

    Threads::runEventLoopFor(Time::MS {200});

    check(primaryOutputOf(probe.connection) == primary);
};

// xcb-cursor reads the theme and its size out of the resource database once,
// when a context is made, so a database that changes is a context that has to
// be made again: without that, the size below never moves.
auto tCursorSizeFollowsTheDatabase =
    test("X11/theCursorSizeFollowsTheResourceDatabase") = []
{
    if (!x11ServerReachable() || !canChangeTheResourceDatabase())
        return;

    auto probe = TestConnection {};

    if (!checkArrived(probe.isValid()))
        return;

    auto input = FakeInput {probe.connection};

    if (!canDriveTheSeat(probe.connection, input))
        return;

    if (!aCursorThemeIsInstalled())
    {
        LOG("No cursor theme is installed on this machine, so xcb-cursor falls "
            "back to the server's cursor font, whose glyphs are one size "
            "whatever the database says; the case is skipped.");
        return;
    }

    auto database = ResourceDatabase {probe.connection};

    database.write("Xcursor.size:\t24\n");

    auto host = InputWindow {200, 150};

    check(host.isUp());

    raiseWindow(probe.connection, host.id());

    const auto origin = rootOriginOf(probe.connection, host.id());

    input.moveTo(origin + Point {80.f, 60.f});

    // The cursor XFixes reports is the root's until the pointer has entered
    // one of ours and the shape that entry set has reached the server, so the
    // first measurement waits for the event that says it did and then for the
    // request behind it.
    Threads::runEventLoopUntil([&] { return !host.content.moves.empty(); },
                               x11TestTimeout);

    if (!checkArrived(!host.content.moves.empty(),
                      "the pointer never entered the window"))
        return;

    Threads::runEventLoopFor(Time::MS {150});

    const auto small = cursorImageSize(probe.connection);

    if (!checkArrived(small.x > 0.f, "XFixes reported no cursor over the window"))
        return;

    database.write("Xcursor.size:\t64\n");

    Threads::runEventLoopUntil(
        [&] { return cursorImageSize(probe.connection).x != small.x; },
        x11TestTimeout);

    const auto large = cursorImageSize(probe.connection);

    check(large.x > small.x,
          "the cursor context was never rebuilt from the new database");
};

auto tPointerMotionReachesTheContentView =
    test("X11/pointerMotionReachesTheContentView") = []
{
    if (!x11ServerReachable())
        return;

    auto host = InputWindow {400, 300};
    auto probe = TestConnection {};
    check(probe.isValid());

    auto input = FakeInput {probe.connection};

    if (!canDriveTheSeat(probe.connection, input))
        return;

    raiseWindow(probe.connection, host.id());

    const auto origin = rootOriginOf(probe.connection, host.id());

    input.moveTo(origin + Point {130.f, 90.f});

    Threads::runEventLoopUntil([&] { return !host.content.moves.empty(); },
                               x11TestTimeout);

    if (!checkArrived(!host.content.moves.empty(),
                      "no mouseMoved reached the content view"))
        return;

    const auto& moved = host.content.moves.back();

    check(moved.pos.x == 130.f);
    check(moved.pos.y == 90.f);
};

auto tAClickDeliversDownAndUp = test("X11/aClickDeliversMouseDownAndMouseUp") = []
{
    if (!x11ServerReachable())
        return;

    auto host = InputWindow {400, 300};
    auto probe = TestConnection {};
    check(probe.isValid());

    auto input = FakeInput {probe.connection};

    if (!canDriveTheSeat(probe.connection, input))
        return;

    raiseWindow(probe.connection, host.id());

    const auto origin = rootOriginOf(probe.connection, host.id());

    input.moveTo(origin + Point {60.f, 70.f});
    Threads::runEventLoopFor(Time::MS {50});

    input.click(1);

    Threads::runEventLoopUntil([&] { return !host.content.ups.empty(); },
                               x11TestTimeout);

    check(host.content.ups.size() == 1);

    if (!checkArrived(host.content.downs.size() == 1, "one press, one mouseDown"))
        return;

    const auto& down = host.content.downs.front();

    check(down.button == MouseButton::Left);
    check(down.clickCount == 1);
    check(down.pos.x == 60.f);
    check(down.pos.y == 70.f);

    if (!host.content.ups.empty())
        check(host.content.ups.front().button == MouseButton::Left);

    // And a second click straight after it is a double.
    input.click(1);

    Threads::runEventLoopUntil([&] { return host.content.downs.size() > 1; },
                               x11TestTimeout);

    if (!checkArrived(host.content.downs.size() == 2))
        return;

    check(host.content.downs.back().clickCount == 2,
          "a second click in the same place was not counted as a double");
};

auto tDraggingDeliversMouseDragged =
    test("X11/movingWithAButtonHeldDeliversMouseDragged") = []
{
    if (!x11ServerReachable())
        return;

    auto host = InputWindow {400, 300};
    auto probe = TestConnection {};
    check(probe.isValid());

    auto input = FakeInput {probe.connection};

    if (!canDriveTheSeat(probe.connection, input))
        return;

    raiseWindow(probe.connection, host.id());

    const auto origin = rootOriginOf(probe.connection, host.id());

    input.moveTo(origin + Point {50.f, 50.f});
    Threads::runEventLoopFor(Time::MS {50});

    input.button(1, true);

    Threads::runEventLoopUntil([&] { return !host.content.downs.empty(); },
                               x11TestTimeout);
    check(!host.content.downs.empty());

    input.moveTo(origin + Point {110.f, 130.f});

    Threads::runEventLoopUntil([&] { return !host.content.drags.empty(); },
                               x11TestTimeout);

    input.button(1, false);
    Threads::runEventLoopUntil([&] { return !host.content.ups.empty(); },
                               x11TestTimeout);

    if (!checkArrived(!host.content.drags.empty(),
                      "a move with a button held was not a drag"))
        return;

    const auto& dragged = host.content.drags.back();

    check(dragged.pos.x == 110.f);
    check(dragged.pos.y == 130.f);
    check(dragged.downPos.x == 50.f);
    check(dragged.downPos.y == 50.f);

    check(host.content.moves.empty() || host.content.moves.back().pos.x != 110.f,
          "the drag was also reported as a plain move");
};

auto tWheelButtonsDeliverScrollEvents =
    test("X11/wheelButtonsDeliverScrollEvents") = []
{
    if (!x11ServerReachable())
        return;

    auto host = InputWindow {400, 300};
    auto probe = TestConnection {};
    check(probe.isValid());

    auto input = FakeInput {probe.connection};

    if (!canDriveTheSeat(probe.connection, input))
        return;

    raiseWindow(probe.connection, host.id());

    const auto origin = rootOriginOf(probe.connection, host.id());

    input.moveTo(origin + Point {80.f, 80.f});
    Threads::runEventLoopFor(Time::MS {50});

    input.click(4);

    Threads::runEventLoopUntil([&] { return !host.content.wheels.empty(); },
                               x11TestTimeout);

    if (!checkArrived(host.content.wheels.size() == 1,
                      "one notch up gave something other than one wheel event"))
        return;

    const auto& up = host.content.wheels.front();

    check(up.delta.y > 0.f, "scrolling up did not give a positive delta");
    check(up.delta.x == 0.f);
    check(!up.preciseScrolling, "a notched wheel is not a precise one");

    input.click(5);

    Threads::runEventLoopUntil([&] { return host.content.wheels.size() > 1; },
                               x11TestTimeout);

    if (!checkArrived(host.content.wheels.size() == 2))
        return;

    check(host.content.wheels.back().delta.y < 0.f,
          "scrolling down did not give a negative delta");
};

auto tSubviewGetsLocalCoordinates =
    test("X11/aSubviewUnderThePointerGetsLocalCoordinates") = []
{
    if (!x11ServerReachable())
        return;

    auto host = InputWindow {400, 300};
    auto child = RecordingView {};

    child.setBounds({100.f, 50.f, 200.f, 100.f});
    host.content.addSubview(child);

    auto probe = TestConnection {};
    check(probe.isValid());

    auto input = FakeInput {probe.connection};

    if (!canDriveTheSeat(probe.connection, input))
        return;

    raiseWindow(probe.connection, host.id());

    const auto origin = rootOriginOf(probe.connection, host.id());

    input.moveTo(origin + Point {150.f, 80.f});
    Threads::runEventLoopFor(Time::MS {50});

    input.click(1);

    Threads::runEventLoopUntil([&] { return !child.ups.empty(); }, x11TestTimeout);

    if (!checkArrived(!child.downs.empty(),
                      "the subview under the pointer was not hit"))
        return;

    check(child.downs.front().pos.x == 50.f);
    check(child.downs.front().pos.y == 30.f);

    check(host.content.downs.empty(),
          "the parent took a press that belonged to its child");
};

auto tPresentingViewStillGetsEvents =
    test("X11/aViewWithAChildWindowStillGetsEvents") = []
{
    if (!x11ServerReachable())
        return;

    auto host = InputWindow {400, 300};
    auto presenter = RecordingPresenter {};

    presenter.setBounds({40.f, 30.f, 200.f, 150.f});
    host.content.addSubview(presenter);

    Threads::runEventLoopUntil([&] { return presenter.available > 0; },
                               x11TestTimeout);
    check(presenter.available == 1, "the view never got a child window");

    auto probe = TestConnection {};
    check(probe.isValid());

    auto input = FakeInput {probe.connection};

    if (!canDriveTheSeat(probe.connection, input))
        return;

    raiseWindow(probe.connection, host.id());

    const auto origin = rootOriginOf(probe.connection, host.id());

    // Deep inside the child window, which selects no input of its own: the
    // press has to propagate up to the toplevel and be hit-tested back down.
    input.moveTo(origin + Point {140.f, 100.f});
    Threads::runEventLoopFor(Time::MS {50});

    input.click(1);

    Threads::runEventLoopUntil([&] { return !presenter.ups.empty(); },
                               x11TestTimeout);

    if (!checkArrived(!presenter.downs.empty(),
                      "an event over a view's child window never reached the view"))
        return;

    check(presenter.downs.front().pos.x == 100.f);
    check(presenter.downs.front().pos.y == 70.f);
};

auto tKeyPressDeliversKeyDownAndUp =
    test("X11/aKeyPressDeliversKeyDownAndKeyUp") = []
{
    if (!x11ServerReachable())
        return;

    auto host = InputWindow {400, 300};
    auto probe = TestConnection {};
    check(probe.isValid());

    auto input = FakeInput {probe.connection};

    if (!canDriveTheSeat(probe.connection, input))
        return;

    raiseWindow(probe.connection, host.id());

    check(takeFocus(probe.connection, host),
          "the window never took the keyboard focus");

    input.key(KEY_A, true);
    input.key(KEY_A, false);

    Threads::runEventLoopUntil([&] { return !host.content.keyUps.empty(); },
                               x11TestTimeout);

    check(!host.content.keyUps.empty());

    if (!checkArrived(!host.content.keyDowns.empty(),
                      "no keyDown reached the content view"))
        return;

    const auto& down = host.content.keyDowns.front();

    check(down.keyCode == KeyCode::A);
    check(down.characters == "a");
    check(down.charactersIgnoringModifiers == "a");
    check(!down.isRepeat);
    check(!down.modifiers.shift);

    if (!host.content.keyUps.empty())
        check(host.content.keyUps.front().keyCode == KeyCode::A);
};

auto tShiftGivesUpperCase =
    test("X11/shiftGivesTheUpperCaseCharacterAndTheModifier") = []
{
    if (!x11ServerReachable())
        return;

    auto host = InputWindow {400, 300};
    auto probe = TestConnection {};
    check(probe.isValid());

    auto input = FakeInput {probe.connection};

    if (!canDriveTheSeat(probe.connection, input))
        return;

    raiseWindow(probe.connection, host.id());

    check(takeFocus(probe.connection, host));

    input.key(KEY_LEFTSHIFT, true);
    input.key(KEY_A, true);
    input.key(KEY_A, false);
    input.key(KEY_LEFTSHIFT, false);

    Threads::runEventLoopUntil(
        [&]
        {
            for (const auto& event: host.content.keyDowns)
                if (event.keyCode == KeyCode::A)
                    return true;

            return false;
        },
        x11TestTimeout);

    auto letter = std::optional<KeyEvent> {};

    for (const auto& event: host.content.keyDowns)
        if (event.keyCode == KeyCode::A)
            letter = event;

    if (!checkArrived(letter.has_value(), "the shifted letter never arrived"))
        return;

    check(letter->modifiers.shift, "the shift the server was holding was lost");
    check(letter->characters == "A");

    // The layout's own answer for the key, with nothing held.
    check(letter->charactersIgnoringModifiers == "a");
};

auto tIsKeyPressedFollowsTheKey =
    test("X11/isKeyPressedFollowsTheKeyAndTheFocus") = []
{
    if (!x11ServerReachable())
        return;

    auto host = InputWindow {400, 300};
    auto probe = TestConnection {};
    check(probe.isValid());

    auto input = FakeInput {probe.connection};

    if (!canDriveTheSeat(probe.connection, input))
        return;

    raiseWindow(probe.connection, host.id());

    check(takeFocus(probe.connection, host));

    check(!Keyboard::isKeyPressed(KeyCode::A));

    input.key(KEY_A, true);

    Threads::runEventLoopUntil([&] { return !host.content.keyDowns.empty(); },
                               x11TestTimeout);

    check(Keyboard::isKeyPressed(KeyCode::A),
          "a key the server says is down reads as up");
    check(Keyboard::isKeyPressed(*host.window, KeyCode::A),
          "the window-scoped query disagreed with the global one");

    input.key(KEY_A, false);

    Threads::runEventLoopUntil([&] { return !host.content.keyUps.empty(); },
                               x11TestTimeout);

    check(!Keyboard::isKeyPressed(KeyCode::A));

    // And what focus loss leaves behind is nothing held: the key-ups go to
    // whoever took the keyboard.
    input.key(KEY_A, true);
    Threads::runEventLoopUntil([&] { return host.content.keyDowns.size() > 1; },
                               x11TestTimeout);
    check(Keyboard::isKeyPressed(KeyCode::A));

    xcb_set_input_focus(probe.connection,
                        XCB_INPUT_FOCUS_POINTER_ROOT,
                        XCB_INPUT_FOCUS_POINTER_ROOT,
                        XCB_CURRENT_TIME);
    xcb_flush(probe.connection);

    Threads::runEventLoopUntil([&] { return !host.active; }, x11TestTimeout);

    check(!host.active, "the window never lost the focus");
    check(!Keyboard::isKeyPressed(KeyCode::A),
          "a key stayed down after the focus went elsewhere");

    input.key(KEY_A, false);
    Threads::runEventLoopFor(Time::MS {50});
};

// The server is what repeats a held key on X11, at whatever rate its own
// controls name; a delay past a second is not this suite's to change.
auto tHoldingAKeyRepeats = test("X11/holdingAKeyRepeats") = []
{
    if (!x11ServerReachable())
        return;

    auto host = InputWindow {400, 300};
    auto probe = TestConnection {};
    check(probe.isValid());

    auto input = FakeInput {probe.connection};

    if (!canDriveTheSeat(probe.connection, input))
        return;

    raiseWindow(probe.connection, host.id());

    check(takeFocus(probe.connection, host));

    input.key(KEY_A, true);

    auto repeated = [&]
    {
        for (const auto& event: host.content.keyDowns)
            if (event.isRepeat)
                return true;

        return false;
    };

    Threads::runEventLoopUntil(repeated, Time::MS {3000});

    input.key(KEY_A, false);
    Threads::runEventLoopFor(Time::MS {50});

    if (!repeated())
    {
        LOG("the server repeated nothing in three seconds: auto-repeat is off "
            "or its delay is longer than that, and the case can assert "
            "nothing");
        return;
    }

    check(host.content.keyDowns.front().isRepeat == false,
          "the first press was reported as a repeat");

    // A repeat is the same key, and it never comes up in between.
    for (const auto& event: host.content.keyDowns)
        check(event.keyCode == KeyCode::A);

    check(host.content.keyUps.size() == 1,
          "the repeats arrived as release/press pairs rather than as repeats");
};

auto tCursorRefreshIsSafe = test("X11/settingACursorAndRefreshingItIsSafe") = []
{
    if (!x11ServerReachable())
        return;

    auto host = InputWindow {400, 300};
    auto probe = TestConnection {};
    check(probe.isValid());

    auto input = FakeInput {probe.connection};

    if (!canDriveTheSeat(probe.connection, input))
        return;

    raiseWindow(probe.connection, host.id());

    const auto origin = rootOriginOf(probe.connection, host.id());

    input.moveTo(origin + Point {70.f, 70.f});
    Threads::runEventLoopUntil([&] { return !host.content.moves.empty(); },
                               x11TestTimeout);

    // Every shape the table knows, so a theme missing one of them is found
    // here rather than in an app.
    for (auto shape: {MouseCursor::IBeam,
                      MouseCursor::PointingHand,
                      MouseCursor::ResizeLeftRight,
                      MouseCursor::ResizeUpDown,
                      MouseCursor::Crosshair,
                      MouseCursor::Default})
    {
        host.content.setMouseCursor(shape);
        linuxRefreshCursor();
    }

    Threads::runEventLoopFor(Time::MS {50});

    // The window is still there, so nothing the server was sent was fatal to
    // the connection.
    check(geometryOf(probe.connection, host.id()).found,
          "the connection died applying a cursor");
    check(host.window->isVisible());
};

auto tClickActivatesAnUnfocusedWindow =
    test("X11/aClickOnAnUnfocusedWindowActivatesIt") = []
{
    if (!x11ServerReachable())
        return;

    auto host = InputWindow {400, 300};
    auto probe = TestConnection {};
    check(probe.isValid());

    auto input = FakeInput {probe.connection};

    if (!canDriveTheSeat(probe.connection, input))
        return;

    raiseWindow(probe.connection, host.id());

    // Nobody has given it the keyboard: with no window manager there is nobody
    // to, and the backend's own focus-on-click rule is the whole mechanism.
    check(!host.active, "the window was active before anything clicked it");

    const auto origin = rootOriginOf(probe.connection, host.id());

    input.moveTo(origin + Point {100.f, 100.f});
    Threads::runEventLoopFor(Time::MS {50});

    input.click(1);

    Threads::runEventLoopUntil([&] { return host.active; }, x11TestTimeout);

    check(host.active, "a click never activated the window");
    check(host.activations >= 1);
};

// Closing a window from inside onActivationChanged is a thing an app is
// allowed to do, and it happens in the middle of the seat's own focus change:
// what the seat was about to go on and do with that window has to notice it is
// gone. No XTest here - SetInputFocus is a request, not a device event, so
// this runs on every server the suite meets.
auto tClosingFromOnActivationChangedIsSafe =
    test("X11/aWindowClosedFromOnActivationChangedIsSafe") = []
{
    if (!x11ServerReachable())
        return;

    auto content = View {};

    auto options = WindowOptions {};
    options.width = 320;
    options.height = 240;
    options.isPrimary = false;
    options.initialPosition = inputWindowOrigin(320, 240);

    auto window = std::optional<Window> {};
    window.emplace(options);

    window->events.onActivationChanged = [&window](bool nowActive)
    {
        if (nowActive)
            window.reset();
    };

    window->setContentView(content);

    // A window manager focuses the window it has just mapped, so it may well
    // have closed itself before this first wait is over.
    Threads::runEventLoopUntil(
        [&] { return !window.has_value() || window->isVisible(); }, x11TestTimeout);

    if (window.has_value())
    {
        auto probe = TestConnection {};

        check(probe.isValid());
        check(window->isVisible());

        const auto id = windowIdOf(*window);

        raiseWindow(probe.connection, id);

        xcb_set_input_focus(
            probe.connection, XCB_INPUT_FOCUS_PARENT, id, XCB_CURRENT_TIME);
        xcb_flush(probe.connection);

        Threads::runEventLoopUntil([&] { return !window.has_value(); },
                                   x11TestTimeout);
    }

    check(!window.has_value(), "the window was never activated, so nothing closed");

    // The window the seat was holding went with it, rather than being left for
    // the next line of the focus change to dereference.
    auto* seat = linuxSeat();

    check(seat != nullptr);
    check(seat->getKeyboardFocus() == nullptr,
          "the seat still names the window that was closed");
};

auto tMouseLockDeliversDeltas =
    test("X11/mouseLockGrabsThePointerAndDeliversDeltas") = []
{
    if (!x11ServerReachable())
        return;

    auto host = InputWindow {400, 300};
    auto probe = TestConnection {};
    check(probe.isValid());

    auto input = FakeInput {probe.connection};

    if (!canDriveTheSeat(probe.connection, input))
        return;

    raiseWindow(probe.connection, host.id());

    check(takeFocus(probe.connection, host));

    const auto origin = rootOriginOf(probe.connection, host.id());

    // Inside already, so the grab's confinement moves nothing and the only
    // warp is the lock's own, to the middle.
    input.moveTo(origin + Point {100.f, 100.f});
    Threads::runEventLoopUntil([&] { return !host.content.moves.empty(); },
                               x11TestTimeout);

    host.window->setMouseLocked(true);
    check(host.window->isMouseLocked());

    Threads::runEventLoopFor(Time::MS {100});

    // The grab is the observable half: nobody else can take the pointer while
    // it is held.
    auto* contested =
        xcb_grab_pointer_reply(probe.connection,
                               xcb_grab_pointer(probe.connection,
                                                1,
                                                host.id(),
                                                XCB_EVENT_MASK_POINTER_MOTION,
                                                XCB_GRAB_MODE_ASYNC,
                                                XCB_GRAB_MODE_ASYNC,
                                                XCB_NONE,
                                                XCB_CURSOR_NONE,
                                                XCB_CURRENT_TIME),
                               nullptr);

    check(contested != nullptr);
    check(contested->status == XCB_GRAB_STATUS_ALREADY_GRABBED,
          "the locked pointer was not grabbed");

    if (contested->status == XCB_GRAB_STATUS_SUCCESS)
        xcb_ungrab_pointer(probe.connection, XCB_CURRENT_TIME);

    std::free(contested);

    host.content.forget();

    // Relative motion, as a device makes it: the pointer is pinned in the
    // middle of the window, and what a lock reports is how far the hand went
    // and not where the pointer ended up.
    input.moveBy({30.f, 20.f});

    Threads::runEventLoopUntil([&] { return !host.content.moves.empty(); },
                               x11TestTimeout);

    if (!checkArrived(!host.content.moves.empty(),
                      "a locked pointer delivered no motion"))
    {
        host.window->setMouseLocked(false);
        return;
    }

    const auto& moved = host.content.moves.front();

    check(moved.delta.x == 30.f, "the delta was not the distance moved");
    check(moved.delta.y == 20.f);
    check(moved.rawDelta.x == 30.f, "rawDelta was not the device's own figure");
    check(moved.rawDelta.y == 20.f);

    host.window->setMouseLocked(false);
    Threads::runEventLoopFor(Time::MS {100});

    auto* released =
        xcb_grab_pointer_reply(probe.connection,
                               xcb_grab_pointer(probe.connection,
                                                1,
                                                host.id(),
                                                XCB_EVENT_MASK_POINTER_MOTION,
                                                XCB_GRAB_MODE_ASYNC,
                                                XCB_GRAB_MODE_ASYNC,
                                                XCB_NONE,
                                                XCB_CURSOR_NONE,
                                                XCB_CURRENT_TIME),
                               nullptr);

    check(released != nullptr);
    check(released->status == XCB_GRAB_STATUS_SUCCESS,
          "unlocking did not let the pointer go");

    if (released->status == XCB_GRAB_STATUS_SUCCESS)
    {
        xcb_ungrab_pointer(probe.connection, XCB_CURRENT_TIME);
        xcb_flush(probe.connection);
    }

    std::free(released);
};

auto tALockedPointerCountsNoWarp =
    test("X11/aLockedPointerCountsNoneOfTheWarpsThatRecentreIt") = []
{
    if (!x11ServerReachable())
        return;

    auto host = InputWindow {400, 300};
    auto probe = TestConnection {};
    check(probe.isValid());

    if (!pointerGoesThroughXinput(probe.connection, host.id()))
    {
        LOG("This server has no XInput 2.1, so a locked pointer is measured "
            "from the warps that recentre it and there is nothing here to "
            "check.");
        return;
    }

    auto input = FakeInput {probe.connection};

    if (!canDriveTheSeat(probe.connection, input))
        return;

    check(takeFocus(probe.connection, host));

    const auto origin = rootOriginOf(probe.connection, host.id());
    const auto root =
        xcb_setup_roots_iterator(xcb_get_setup(probe.connection)).data->root;

    input.moveTo(origin + Point {100.f, 100.f});
    Threads::runEventLoopUntil([&] { return !host.content.moves.empty(); },
                               x11TestTimeout);

    host.window->setMouseLocked(true);
    Threads::runEventLoopFor(Time::MS {100});

    host.content.forget();

    // A warp is the one thing that moves a pointer without a device having
    // moved, which is what the lock itself keeps doing: it makes no raw event,
    // and so it must make no delta either.
    input.moveTo(origin + Point {230.f, 170.f});
    Threads::runEventLoopFor(Time::MS {200});

    check(host.content.moves.empty(),
          "a warp was counted as the hand moving the pointer");
    check(host.content.drags.empty());

    const auto centre = origin + Point {200.f, 150.f};
    const auto pinned = pointerPosition(probe.connection, root);

    check(pinned.x == centre.x && pinned.y == centre.y,
          "the lock did not put the pointer back in the middle");

    // And the device itself, which is counted exactly once.
    input.moveBy({10.f, -6.f});

    Threads::runEventLoopUntil([&] { return !host.content.moves.empty(); },
                               x11TestTimeout);
    Threads::runEventLoopFor(Time::MS {100});

    if (!checkArrived(host.content.moves.size() == 1,
                      "one device movement gave something other than one move"))
    {
        host.window->setMouseLocked(false);
        return;
    }

    check(host.content.moves.front().delta.x == 10.f);
    check(host.content.moves.front().delta.y == -6.f);

    host.window->setMouseLocked(false);
};

auto tThePointerIsSelectedThroughXinput =
    test("X11/thePointerIsSelectedThroughXinput") = []
{
    if (!x11ServerReachable())
        return;

    auto host = InputWindow {400, 300};
    auto probe = TestConnection {};
    check(probe.isValid());

    if (!pointerGoesThroughXinput(probe.connection, host.id()))
    {
        LOG("This server has no XInput 2.1, so the core pointer is the whole "
            "of the seat and there is nothing here to check.");
        return;
    }

    auto input = FakeInput {probe.connection};

    if (!canDriveTheSeat(probe.connection, input))
        return;

    raiseWindow(probe.connection, host.id());

    const auto origin = rootOriginOf(probe.connection, host.id());

    input.moveTo(origin + Point {70.f, 60.f});
    Threads::runEventLoopFor(Time::MS {50});

    host.content.forget();

    input.click(1);

    Threads::runEventLoopUntil([&] { return !host.content.ups.empty(); },
                               x11TestTimeout);
    Threads::runEventLoopFor(Time::MS {100});

    check(host.content.downs.size() == 1, "the press arrived more than once");
    check(host.content.ups.size() == 1, "the release arrived more than once");
};

// The fallback a server older than XI2 2.1 leaves, which EACP_X11_NO_XI2 asks
// for by hand. It has to be set before the connection is opened, and opening
// it is what x11ServerReachable does: under ctest, where every case is a
// process of its own, this case's first line is early enough, and a run of the
// whole binary by hand finds the connection already open and says so.
auto tWithoutXinputTheCorePointerStillArrives =
    test("X11/withoutXinputTheCorePointerStillArrives") = []
{
    ::setenv("EACP_X11_NO_XI2", "1", 1);

    if (!x11ServerReachable())
        return;

    auto host = InputWindow {400, 300};
    auto probe = TestConnection {};
    check(probe.isValid());

    if (pointerGoesThroughXinput(probe.connection, host.id()))
    {
        LOG("The connection was already open when EACP_X11_NO_XI2 was set, "
            "which is a run of the whole binary in one process: the fallback "
            "cannot be reached from here. ctest runs this case on its own.");
        return;
    }

    auto input = FakeInput {probe.connection};

    if (!canDriveTheSeat(probe.connection, input))
        return;

    raiseWindow(probe.connection, host.id());

    const auto origin = rootOriginOf(probe.connection, host.id());

    input.moveTo(origin + Point {60.f, 50.f});

    Threads::runEventLoopUntil([&] { return !host.content.moves.empty(); },
                               x11TestTimeout);
    check(!host.content.moves.empty(), "core motion reached nothing");

    input.click(1);

    Threads::runEventLoopUntil([&] { return !host.content.ups.empty(); },
                               x11TestTimeout);

    check(host.content.downs.size() == 1, "a core press reached nothing");

    input.click(4);

    Threads::runEventLoopUntil([&] { return !host.content.wheels.empty(); },
                               x11TestTimeout);

    if (!checkArrived(host.content.wheels.size() == 1,
                      "a core wheel button gave other than one notch"))
        return;

    check(host.content.wheels.front().delta.y > 0.f);
};

// No display needed: a scroll valuator is arithmetic, and the one thing no
// server this suite can run on has is a device with one. Xvfb's virtual
// pointer has no scroll class at all and XWayland owns its seat, so the sums
// are checked here and the wheel-button path beside them is what the server
// actually drives.
auto tScrollValuatorsBecomeNotches = test("X11/aScrollValuatorCountsInClicks") = []
{
    auto axis = X11ScrollAxis {true, 3, 15.0, 0.0, false};

    // Wherever the device happens to be counting from is not a scroll.
    check(axis.step(1234.0) == 0.f, "the first value was taken for a scroll");

    check(axis.step(1249.0) == 1.f, "one increment was not one click");
    check(axis.step(1234.0) == -1.f, "counting back was not a click back");

    // A trackpad sends a fraction of a click, which stays a fraction: X11
    // measures scrolling in clicks and never in pixels.
    check(axis.step(1239.0) == 1.f / 3.f);

    // A device whose class named no increment cannot be divided by it.
    auto broken = X11ScrollAxis {true, 2, 0.0, 0.0, false};

    check(broken.step(4.0) == 0.f);
    check(broken.step(40.0) == 0.f, "a zero increment scrolled something");
};

auto tEmulatedWheelButtonsAreDropped =
    test("X11/anEmulatedWheelButtonIsNotAnotherNotch") = []
{
    constexpr auto emulated =
        (uint32_t) XCB_INPUT_POINTER_EVENT_FLAGS_POINTER_EMULATED;

    // The four the server makes up for clients that know nothing of valuators.
    for (auto button = 4u; button <= 7u; ++button)
        check(x11IsEmulatedWheelButton(emulated, button),
              "an emulated wheel button was let through as a notch");

    // The same buttons from a device with no scroll valuator, which is the
    // only thing that ever scrolls under Xvfb or in most VMs.
    for (auto button = 4u; button <= 7u; ++button)
        check(!x11IsEmulatedWheelButton(0, button));

    // An emulated press that is not a wheel is a touch, and that one is wanted.
    check(!x11IsEmulatedWheelButton(emulated, 1));
    check(!x11IsEmulatedWheelButton(emulated, 8));
};

// Last in the file on purpose: the connection it closes is the process's only
// one, so every case after it in a direct run would find itself surfaceless.
// ctest gives each case a process of its own, which is where this is honest.
auto tLosingTheConnectionTearsTheWindowsDown =
    test("X11/zLosingTheConnectionTearsTheWindowsDown") = []
{
    if (!x11ServerReachable())
        return;

    auto host = ServerWindow {640, 400};
    auto presenter = PresentingView {};

    presenter.setBounds({0.f, 0.f, 320.f, 240.f});
    host.content.addSubview(presenter);

    Threads::runEventLoopUntil([&] { return presenter.available > 0; },
                               x11TestTimeout);
    check(presenter.available == 1);

    // KillClient on a resource of ours makes the server close this client's
    // connection and nobody else's - the X11 twin of the protocol error the
    // Wayland case provokes. Killing the server itself would take every other
    // case sharing the display down with it.
    auto* connection =
        static_cast<xcb_connection_t*>(presenter.record.handle.connection);

    check(connection != nullptr);

    xcb_kill_client(connection, presenter.record.handle.window);
    xcb_flush(connection);

    Threads::runEventLoopUntil([&] { return presenter.lost > 0; }, x11TestTimeout);

    check(presenter.lost == 1, "onLost never fired when the connection died");
    check(!presenter.hasSurface());
    check(!host.window->isVisible(), "the window still reported itself mapped");

    // The process carries on, and what it makes now is what a headless build
    // makes: a window with no surface behind it.
    auto options = WindowOptions {};
    options.isPrimary = false;

    auto afterwards = Window {options};

    check(afterwards.getHandle() == nullptr,
          "a window built after the loss still reached the server");
};

namespace
{
int x11TestArgCount = 0;
char** x11TestArgValues = nullptr;
int x11TestExitCode = 0;

void runX11Tests()
{
    x11TestExitCode = nano::run(x11TestArgCount, x11TestArgValues);
}
} // namespace

// Apps::run wraps it: runEventLoopUntil needs a bootstrapped loop.
int main(int argc, char* argv[])
{
    x11TestArgCount = argc;
    x11TestArgValues = argv;

    // Defaulted, not forced: Scripts/with-xvfb has already set it, and a run
    // by hand on a Wayland desktop should still reach XWayland.
    ::setenv("EACP_WINDOW_SYSTEM", "x11", 0);

    eacp::Apps::run(runX11Tests);

    return x11TestExitCode;
}
