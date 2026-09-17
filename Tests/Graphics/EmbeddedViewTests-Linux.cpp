#include "Common.h"

#include <eacp/Core/App/AppEnvironment.h>
#include <eacp/Core/Threads/EventLoop-Linux.h>
#include <eacp/Core/Utils/Environment.h>
#include <eacp/Graphics/Graphics/Keyboard.h>
#include <eacp/Graphics/View/View-Linux.h>
#include <eacp/Graphics/Window/EmbeddedView.h>

#include <xcb/xcb.h>

#if EACP_HAS_XTEST
#include <xcb/xtest.h>
#endif

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <source_location>
#include <linux/input-event-codes.h>
#include <optional>
#include <poll.h>
#include <string>
#include <thread>
#include <unistd.h>
#include <vector>

// The Linux EmbeddedView against a real server, with the test playing host:
// its own xcb connection creates the parent window, and eacp is driven only by
// poll()ing getEventLoopFd() and calling pumpEventLoop() - never runEventLoop,
// because a DAW's loop is the host's and eacp's never runs at all (plan.md D1,
// D6). That is also why this is a binary with a plain main: pumpEventLoop
// refuses re-entry, so every case inside Apps::run would pump nothing.
//
// Every case self-skips without an X server, unless EACP_REQUIRE_DISPLAY=1,
// and the cases that drive the seat skip again under Xwayland, whose pointer
// and keyboard belong to the compositor. Scripts/with-xvfb runs them for real.

using namespace nano;
using namespace eacp;
using namespace eacp::Graphics;

namespace
{
constexpr auto embeddedTestTimeout = Time::MS {5000};

// Long enough that a round trip on the test's own connection is not waited out
// in one go: a state this file checks for often arrives with no event of
// eacp's behind it, so the predicate has to be asked again whatever the loop
// is doing.
constexpr auto embeddedPumpSlice = 8;

bool x11ServerReachable()
{
    if (Apps::getAppEnvironment().headless)
        return false;

    if (getEnvValue("EACP_WINDOW_SYSTEM") != "x11")
        return false;

    return !getEnvValue("DISPLAY").empty();
}

// Everything a VST3 or CLAP host offers a plugin: one descriptor to wait on
// and one call to make when it fires.
struct FakeHost
{
    template <typename Predicate>
    bool pumpUntil(Predicate ready, Time::MS timeout = embeddedTestTimeout)
    {
        auto deadline = Time::Deadline {timeout};

        while (!ready())
        {
            if (deadline.expired())
                return ready();

            const auto remaining = (int) deadline.remaining().count;

            auto fds = pollfd {Threads::getEventLoopFd(), POLLIN, 0};
            ::poll(&fds, 1, std::min(remaining, embeddedPumpSlice));

            Threads::pumpEventLoop();
        }

        return true;
    }

    void pumpFor(Time::MS howLong)
    {
        pumpUntil([] { return false; }, howLong);
    }
};

// A connection of the test's own: what it asserts about a window comes from
// the server rather than from the backend under test, and the host window it
// creates belongs to another client, exactly as a DAW's does.
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

    xcb_screen_t* screen() const
    {
        return xcb_setup_roots_iterator(xcb_get_setup(connection)).data;
    }

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

xcb_window_t parentOf(xcb_connection_t* connection, xcb_window_t window)
{
    if (window == XCB_NONE)
        return XCB_NONE;

    auto* reply = xcb_query_tree_reply(
        connection, xcb_query_tree(connection, window), nullptr);

    if (reply == nullptr)
        return XCB_NONE;

    const auto parent = reply->parent;
    std::free(reply);

    return parent;
}

bool isMapped(xcb_connection_t* connection, xcb_window_t window)
{
    if (window == XCB_NONE)
        return false;

    auto* reply = xcb_get_window_attributes_reply(
        connection, xcb_get_window_attributes(connection, window), nullptr);

    if (reply == nullptr)
        return false;

    const auto mapped = reply->map_state != XCB_MAP_STATE_UNMAPPED;
    std::free(reply);

    return mapped;
}

xcb_window_t focusedWindow(xcb_connection_t* connection)
{
    auto* reply = xcb_get_input_focus_reply(
        connection, xcb_get_input_focus(connection), nullptr);

    if (reply == nullptr)
        return XCB_NONE;

    const auto focus = reply->focus;
    std::free(reply);

    return focus;
}

Point rootOriginOf(xcb_connection_t* connection, xcb_window_t window)
{
    auto* screen = xcb_setup_roots_iterator(xcb_get_setup(connection)).data;

    auto* reply = xcb_translate_coordinates_reply(
        connection,
        xcb_translate_coordinates(connection, window, screen->root, 0, 0),
        nullptr);

    if (reply == nullptr)
        return {};

    const auto origin = Point {(float) reply->dst_x, (float) reply->dst_y};
    std::free(reply);

    return origin;
}

// The window every case is embedded into: somebody else's, on somebody else's
// connection, in the far corner where no other suite's window lands.
struct HostWindow
{
    HostWindow(TestConnection& connectionToUse, int width, int height)
        : connection(connectionToUse.connection)
    {
        auto* screen = connectionToUse.screen();

        if (screen == nullptr)
            return;

        window = xcb_generate_id(connection);

        const auto x = std::max((int) screen->width_in_pixels - width - 40, 0);
        const auto y = std::max((int) screen->height_in_pixels - height - 40, 0);

        const uint32_t values[] = {screen->black_pixel,
                                   XCB_EVENT_MASK_STRUCTURE_NOTIFY};

        xcb_create_window(connection,
                          XCB_COPY_FROM_PARENT,
                          window,
                          screen->root,
                          (int16_t) x,
                          (int16_t) y,
                          (uint16_t) width,
                          (uint16_t) height,
                          0,
                          XCB_WINDOW_CLASS_INPUT_OUTPUT,
                          screen->root_visual,
                          XCB_CW_BACK_PIXEL | XCB_CW_EVENT_MASK,
                          values);

        static constexpr auto title = "eacp embedded view tests";

        xcb_change_property(connection,
                            XCB_PROP_MODE_REPLACE,
                            window,
                            XCB_ATOM_WM_NAME,
                            XCB_ATOM_STRING,
                            8,
                            (uint32_t) std::strlen(title),
                            title);

        xcb_map_window(connection, window);

        raise();
    }

    ~HostWindow()
    {
        if (window == XCB_NONE)
            return;

        xcb_destroy_window(connection, window);
        xcb_flush(connection);
    }

    // Nothing else on this display may be over the host: with no window
    // manager the stack is the client's own to change, and a desktop session's
    // window manager restacks on its own time.
    void raise()
    {
        const uint32_t above = XCB_STACK_MODE_ABOVE;

        xcb_configure_window(
            connection, window, XCB_CONFIG_WINDOW_STACK_MODE, &above);
        xcb_flush(connection);
    }

    // What a host does when it closes the plugin's window rather than the
    // whole application: our child goes with it, as every inferior does.
    void destroyNow()
    {
        if (window == XCB_NONE)
            return;

        xcb_destroy_window(connection, window);
        xcb_flush(connection);

        window = XCB_NONE;
    }

    void resizeTo(int width, int height)
    {
        const uint32_t values[] = {(uint32_t) width, (uint32_t) height};

        xcb_configure_window(connection,
                             window,
                             XCB_CONFIG_WINDOW_WIDTH | XCB_CONFIG_WINDOW_HEIGHT,
                             values);
        xcb_flush(connection);
    }

    bool isUp() const { return window != XCB_NONE && isMapped(connection, window); }

    TestGeometry geometry() const { return geometryOf(connection, window); }

    xcb_connection_t* connection = nullptr;
    xcb_window_t window = XCB_NONE;
};

// A view that remembers what it was told, and nothing else. Mouse events only
// reach a view that asks for them.
struct RecordingView : View
{
    RecordingView() { getProperties().handlesMouseEvents = true; }

    void mouseDown(const MouseEvent& event) override { downs.push_back(event); }
    void mouseUp(const MouseEvent& event) override { ups.push_back(event); }
    void keyDown(const KeyEvent& event) override { keyDowns.push_back(event); }
    void keyUp(const KeyEvent& event) override { keyUps.push_back(event); }

    std::vector<MouseEvent> downs;
    std::vector<MouseEvent> ups;
    std::vector<KeyEvent> keyDowns;
    std::vector<KeyEvent> keyUps;
};

// What a GPUView is to the window backend: a view with a native child window
// of its own under it. The GPU module itself is one layer further up, and
// Tests/GPU's Present/anEmbeddedViewPresentsIntoItsHost is where a real
// swapchain goes into one of these.
struct PresentingView : RecordingView
{
    PresentingView()
        : record(requestViewSurface(*this))
    {
        record.onAvailable = [this] { ++available; };
        record.onLost = [this] { ++lost; };
        record.onResized = [this] { ++resized; };
    }

    ~PresentingView() override
    {
        // ~View fires onLost when this object's own members are already gone.
        record.onAvailable = [] {};
        record.onLost = [] {};
        record.onResized = [] {};
    }

    bool hasSurface() const { return record.handle.isValid(); }

    xcb_window_t id() const { return (xcb_window_t) record.handle.window; }

    ViewSurface& record;
    int available = 0;
    int lost = 0;
    int resized = 0;
};

// The host, its window and the surface inside it, in the order they have to go
// away in: the content view outlives the surface that was drawn against it.
struct EmbeddedHost
{
    EmbeddedHost(int width = 600, int height = 400)
        : host(connection, width, height)
    {
        pump.pumpUntil([this] { return host.isUp(); });

        view.emplace(reinterpret_cast<void*>((uintptr_t) host.window));
        view->setContentView(content);

        pump.pumpUntil([this] { return isUp(); });

        waitForHostToSettle();
    }

    // A window manager answers a map with a configure of its own, on its own
    // time, and a surface that has not been placed follows the host: a size a
    // case sets while one of those is still in flight would be overwritten by
    // it. Nothing is asserted here - a host that never settles is the window
    // manager's business, and the case that follows says so itself.

    ~EmbeddedHost() { view.reset(); }

    bool isUp() { return id() != XCB_NONE && isMapped(connection.connection, id()); }

    // Not const: an EmbeddedView's handle is the surface's own, and asking for
    // it is not a const question anywhere.
    xcb_window_t id()
    {
        return view ? (xcb_window_t) (uintptr_t) view->getHandle() : XCB_NONE;
    }

    TestGeometry childGeometry() { return geometryOf(connection.connection, id()); }

    void waitForHostToSettle()
    {
        auto deadline = Time::Deadline {embeddedTestTimeout};
        auto last = host.geometry();

        while (!deadline.expired())
        {
            pump.pumpFor(Time::MS {120});

            const auto now = host.geometry();

            if (now.found && now.width == last.width && now.height == last.height)
                return;

            last = now;
        }
    }

    // Waits for the server to agree, which is a round trip on the test's own
    // connection and so has nothing to do with what eacp has processed.
    bool waitForChildSize(int width, int height)
    {
        return pump.pumpUntil(
            [&]
            {
                const auto geometry = childGeometry();

                return geometry.found && geometry.width == width
                       && geometry.height == height;
            });
    }

    RecordingView content;
    TestConnection connection;
    HostWindow host;
    FakeHost pump;
    std::optional<EmbeddedView> view;
};

// The protocol errors the backend logs go through LOG like everything else, so
// a case that must not provoke one reads them back out of a log file of its
// own. Nothing else in this process writes there.
struct LogCapture
{
    LogCapture()
        : path(std::filesystem::temp_directory_path()
               / ("eacp-embedded-" + std::to_string(::getpid()) + ".log"))
    {
        std::error_code ignored;
        std::filesystem::remove(path, ignored);

        setLogFile(path.string());
    }

    ~LogCapture()
    {
        setLogFile({});

        std::error_code ignored;
        std::filesystem::remove(path, ignored);
    }

    std::string read() const
    {
        auto file = std::ifstream {path};

        return {std::istreambuf_iterator<char> {file},
                std::istreambuf_iterator<char> {}};
    }

    std::filesystem::path path;
};

// Every thread this process has, which is how a pacing timer that outlived the
// view it was pacing shows itself: one thread per Threads::Timer.
int threadCount()
{
    auto count = 0;

    for (const auto& entry: std::filesystem::directory_iterator {"/proc/self/task"})
        count += entry.is_directory() ? 1 : 0;

    return count;
}

bool threadCountSettlesTo(int expected)
{
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds {2};

    while (threadCount() != expected)
    {
        if (std::chrono::steady_clock::now() > deadline)
            return false;

        std::this_thread::sleep_for(std::chrono::milliseconds {1});
    }

    return true;
}

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

    // A warp rather than XTest's own motion, for the reason X11WindowTests
    // gives: it is the one thing that moves the pointer on both servers.
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

    void button(uint8_t number, bool pressed)
    {
        fake(pressed ? XCB_BUTTON_PRESS : XCB_BUTTON_RELEASE, number);
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

// Under Xwayland the pointer and the keyboard belong to the compositor, so an
// X client's synthetic input reaches nothing that has not grabbed. The
// extension is on no other server, which makes it the test.
bool serverOwnsItsSeat(xcb_connection_t* probe)
{
    auto* reply = xcb_query_extension_reply(
        probe, xcb_query_extension(probe, 8, "XWAYLAND"), nullptr);

    const auto compositorOwned = reply != nullptr && reply->present != 0;
    std::free(reply);

    return !compositorOwned;
}

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
} // namespace

// The one case that does not self-skip.
auto tServerIsPresentWhenRequired =
    test("EmbeddedView/aServerIsPresentWhenRequired") = []
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

// No server to ask, no id to be given: what the host gets back is the surface
// a headless build makes, and everything on it still answers.
auto tNoHostIdIsSurfaceless =
    test("EmbeddedView/aSurfaceWithNoHostIdIsHeadlessAndSafe") = []
{
    auto content = RecordingView {};
    auto embedded = EmbeddedView {nullptr, {320, 200}};

    embedded.setContentView(content);

    check(embedded.getHandle() == nullptr, "a surface was made with no host id");

    // Laid out at the options' size, with nothing to lay it out on: the same
    // thing a headless toplevel gives its content view.
    check(content.getBounds().w == 320.f,
          "the content view of a surfaceless EmbeddedView was left at no size");
    check(content.getBounds().h == 200.f);

    embedded.setBounds({10.f, 10.f, 100.f, 80.f});

    check(content.getBounds().w == 100.f, "a placed surface did not lay out");
    check(content.getBounds().h == 80.f);

    embedded.setSize(200, 150);

    check(content.getBounds().w == 200.f);
    check(content.getBounds().h == 150.f);

    embedded.setPixelsPerPoint(2.f);

    // A placed surface keeps the points it was placed at and would cover twice
    // as many pixels; only one that is filling its host trades the other way.
    check(content.getBounds().w == 200.f);
    check(content.getBounds().h == 150.f);

    embedded.setVisible(false);
    embedded.setVisible(true);

    check(embedded.getBounds().w == 200.f);
    check(embedded.isVisible());
};

auto tChildFillsTheHost =
    test("EmbeddedView/theSurfaceIsAChildOfTheHostFillingIt") = []
{
    if (!x11ServerReachable())
        return;

    auto embedded = EmbeddedHost {600, 400};

    check(embedded.host.isUp(), "the host window never came up");

    if (!checkArrived(embedded.id() != XCB_NONE, "no child window was made"))
        return;

    check(parentOf(embedded.connection.connection, embedded.id())
              == embedded.host.window,
          "the surface is not a child of the host's window");

    const auto geometry = embedded.childGeometry();
    const auto host = embedded.host.geometry();

    // Against the host's own geometry rather than the size it was asked for:
    // what a window manager gave the host is the host's business, and what is
    // being checked is that the surface fills it.
    check(geometry.found);
    check(host.found);
    check(host.width > 0);
    check(geometry.x == 0);
    check(geometry.y == 0);
    check(geometry.width == host.width);
    check(geometry.height == host.height);

    // In points, which are pixels until a host says otherwise.
    check(embedded.content.getBounds().w == (float) host.width);
    check(embedded.content.getBounds().h == (float) host.height);
};

auto tSetBoundsPlacesTheSurface =
    test("EmbeddedView/setBoundsPlacesTheSurfaceInTheHost") = []
{
    if (!x11ServerReachable())
        return;

    auto embedded = EmbeddedHost {600, 400};

    if (!checkArrived(embedded.id() != XCB_NONE))
        return;

    embedded.view->setBounds({20.f, 30.f, 200.f, 150.f});

    check(embedded.waitForChildSize(200, 150),
          "the server never gave the surface the size it was placed at");

    const auto geometry = embedded.childGeometry();

    check(geometry.x == 20);
    check(geometry.y == 30);

    check(embedded.content.getBounds().w == 200.f);
    check(embedded.content.getBounds().h == 150.f);
    check(embedded.view->getBounds().x == 20.f);
};

auto tSetSizeResizesAboutTheTopLeft =
    test("EmbeddedView/setSizeResizesAboutTheTopLeft") = []
{
    if (!x11ServerReachable())
        return;

    auto embedded = EmbeddedHost {600, 400};

    if (!checkArrived(embedded.id() != XCB_NONE))
        return;

    const auto before = embedded.host.geometry();

    embedded.view->setSize(320, 240);

    // The whole geometry inside the wait: setSize leaves the surface
    // following, so a late configure of the host's would put it back and a
    // size read after the wait could be either one.
    check(embedded.pump.pumpUntil(
              [&]
              {
                  const auto geometry = embedded.childGeometry();

                  return geometry.found && geometry.width == 320
                         && geometry.height == 240 && geometry.x == 0
                         && geometry.y == 0;
              }),
          "the server never gave the surface the size it was set to");

    const auto after = embedded.host.geometry();

    check(after.width == before.width && after.height == before.height,
          "the host resized under the case, which is not what it measures");

    check(embedded.content.getBounds().w == 320.f);
    check(embedded.content.getBounds().h == 240.f);
};

auto tSurfaceFollowsTheHost =
    test("EmbeddedView/theSurfaceFollowsTheHostUntilItIsPlaced") = []
{
    if (!x11ServerReachable())
        return;

    auto embedded = EmbeddedHost {600, 400};

    if (!checkArrived(embedded.id() != XCB_NONE))
        return;

    // The size the host ends up at is the window manager's to decide - mutter
    // hands out a pixel more than it was asked for - so what is checked is
    // that the surface is the host's size, whatever the host's size became.
    embedded.host.resizeTo(500, 300);

    check(embedded.pump.pumpUntil(
              [&]
              {
                  const auto host = embedded.host.geometry();
                  const auto child = embedded.childGeometry();

                  return host.found && child.found && host.width != 600
                         && child.width == host.width && child.height == host.height;
              }),
          "the surface did not follow the host's resize");

    const auto followed = embedded.host.geometry();

    check(embedded.content.getBounds().w == (float) followed.width);
    check(embedded.content.getBounds().h == (float) followed.height);

    // From here the host's layout is the authority, and the automatic sizing
    // is off for good.
    embedded.view->setBounds({0.f, 0.f, 200.f, 100.f});

    check(embedded.waitForChildSize(200, 100));

    embedded.host.resizeTo(640, 480);

    // Long enough for a configure to have been seen and acted on.
    embedded.pump.pumpFor(Time::MS {300});

    const auto geometry = embedded.childGeometry();

    check(geometry.width == 200, "a placed surface followed the host anyway");
    check(geometry.height == 100);
    check(embedded.content.getBounds().w == 200.f);
};

auto tPixelsPerPointHalvesThePoints =
    test("EmbeddedView/pixelsPerPointHalvesTheContentPointSize") = []
{
    if (!x11ServerReachable())
        return;

    auto embedded = EmbeddedHost {600, 400};

    if (!checkArrived(embedded.id() != XCB_NONE))
        return;

    embedded.view->setPixelsPerPoint(2.f);

    embedded.pump.pumpFor(Time::MS {100});

    // The host's window is the same number of pixels as it was: what changed
    // is how many points they are worth.
    const auto geometry = embedded.childGeometry();

    check(geometry.width == 600);
    check(geometry.height == 400);

    check(embedded.content.getBounds().w == 300.f,
          "the content view was not re-laid out in the host's points");
    check(embedded.content.getBounds().h == 200.f);
};

auto tPixelsPerPointPlacesInPixels =
    test("EmbeddedView/pixelsPerPointPlacesTheSurfaceInPixels") = []
{
    if (!x11ServerReachable())
        return;

    auto embedded = EmbeddedHost {600, 400};

    if (!checkArrived(embedded.id() != XCB_NONE))
        return;

    embedded.view->setPixelsPerPoint(2.f);
    embedded.view->setBounds({10.f, 20.f, 150.f, 100.f});

    check(embedded.waitForChildSize(300, 200),
          "a placed surface was not sized in the host's pixels");

    const auto geometry = embedded.childGeometry();

    check(geometry.x == 20);
    check(geometry.y == 40);

    check(embedded.content.getBounds().w == 150.f);
    check(embedded.content.getBounds().h == 100.f);
};

auto tVisibilityMapsAndUnmaps =
    test("EmbeddedView/setVisibleUnmapsAndMapsTheSurface") = []
{
    if (!x11ServerReachable())
        return;

    auto embedded = EmbeddedHost {400, 300};

    if (!checkArrived(embedded.id() != XCB_NONE))
        return;

    auto* probe = embedded.connection.connection;
    const auto child = embedded.id();

    embedded.view->setVisible(false);

    check(embedded.pump.pumpUntil([&] { return !isMapped(probe, child); }),
          "the surface stayed mapped after setVisible(false)");

    check(!embedded.view->isVisible());

    embedded.view->setVisible(true);

    check(embedded.pump.pumpUntil([&] { return isMapped(probe, child); }),
          "the surface never came back after setVisible(true)");

    // The id is the same one: hiding a surface does not destroy it.
    check(embedded.id() == child);
};

auto tPresentingViewGetsAChildWindow =
    test("EmbeddedView/aPresentingViewGetsASurfaceInsideTheHost") = []
{
    if (!x11ServerReachable())
        return;

    // The subview goes after the surface it will be inside, so it is the
    // first of the two to go away.
    auto embedded = EmbeddedHost {400, 300};
    auto presenter = PresentingView {};

    presenter.setBounds({40.f, 30.f, 200.f, 150.f});
    embedded.content.addSubview(presenter);

    check(embedded.pump.pumpUntil([&] { return presenter.available > 0; }),
          "a view inside the surface never got a child window");

    if (!checkArrived(presenter.hasSurface()))
        return;

    auto* probe = embedded.connection.connection;

    // Waited for rather than read once: the window was asked for on eacp's
    // connection and is being asked about on the test's, and nothing orders
    // one client's request against another's.
    const auto child = embedded.id();

    check(embedded.pump.pumpUntil(
              [&] { return parentOf(probe, presenter.id()) == child; }),
          "the presenting view's window is not inside the embedded surface");

    const auto geometry = geometryOf(probe, presenter.id());

    check(geometry.width == 200);
    check(geometry.height == 150);

    check(presenter.record.pixelWidth == 200);
    check(presenter.record.pixelHeight == 150);
};

// What a host does when it closes a plugin's window: the surface goes while
// everything inside it is still live.
auto tTeardownWithAPresenterInside =
    test("EmbeddedView/destroyingItWithAPresentingViewInsideIsClean") = []
{
    if (!x11ServerReachable())
        return;

    auto embedded = EmbeddedHost {400, 300};
    auto presenter = PresentingView {};

    embedded.content.addSubview(presenter);
    presenter.setBounds({0.f, 0.f, 400.f, 300.f});

    check(embedded.pump.pumpUntil([&] { return presenter.available > 0; }));

    auto* probe = embedded.connection.connection;
    const auto child = embedded.id();
    const auto presented = presenter.id();

    embedded.view.reset();

    check(presenter.lost == 1, "onLost did not fire as the surface went away");
    check(!presenter.hasSurface());

    check(embedded.pump.pumpUntil([&] { return !geometryOf(probe, child).found; }),
          "the surface's window outlived the EmbeddedView");
    check(!geometryOf(probe, presented).found);

    // And the host's own window is untouched.
    check(embedded.host.isUp());
};

// The other way a surface ends: the host destroys the window it handed over,
// and the server destroys everything inside it - our child, and every
// presenting view's window under it. Nothing of ours may then ask the server
// to destroy those again, which was one BadWindow logged per presenting view.
auto tHostDestroyingItsWindowIsClean =
    test("EmbeddedView/theHostDestroyingItsWindowIsClean") = []
{
    if (!x11ServerReachable())
        return;

    auto embedded = EmbeddedHost {400, 300};
    auto first = PresentingView {};
    auto second = PresentingView {};

    embedded.content.addSubview(first);
    embedded.content.addSubview(second);

    first.setBounds({0.f, 0.f, 200.f, 300.f});
    second.setBounds({200.f, 0.f, 200.f, 300.f});

    check(embedded.pump.pumpUntil(
        [&] { return first.available > 0 && second.available > 0; }));

    auto log = LogCapture {};

    embedded.host.destroyNow();

    check(embedded.pump.pumpUntil([&] { return first.lost > 0 && second.lost > 0; }),
          "the view surfaces were never told their windows had gone");

    check(!first.hasSurface());
    check(!second.hasSurface());
    check(embedded.id() == XCB_NONE,
          "the surface still reports a window the server has destroyed");

    // And everything still answers, on the surface a headless build has.
    embedded.view->setBounds({0.f, 0.f, 100.f, 100.f});
    embedded.view->setVisible(false);
    embedded.view->setVisible(true);

    embedded.pump.pumpFor(Time::MS {100});

    // DestroyWindow specifically, rather than any error at all: a request of
    // ours already on its way to the server when another client destroyed the
    // window it names is answered with an error whatever we do, and that race
    // is the host's timing. Asking for a window the server has already reaped
    // to be destroyed is ours, and there must be none of it.
    check(log.read().find("from request 4.") == std::string::npos,
          "a window the server had already destroyed was destroyed again");
};

// A pacing timer is a thread, and a plugin is dlclosed the moment its UI is
// destroyed: one still ticking is a timer firing into unmapped code.
auto tPacerLeavesNoThreadBehind =
    test("EmbeddedView/aPacedViewLeavesNoThreadBehindWhenItGoes") = []
{
    if (!x11ServerReachable())
        return;

    auto embedded = EmbeddedHost {400, 300};

    const auto idle = threadCount();

    {
        auto presenter = PresentingView {};

        embedded.content.addSubview(presenter);
        presenter.setBounds({0.f, 0.f, 400.f, 300.f});

        check(embedded.pump.pumpUntil([&] { return presenter.available > 0; }));

        // What a GPUView asks for on every frame it presents.
        presenter.record.requestFrameCallback();

        check(embedded.pump.pumpUntil([&] { return threadCount() > idle; }),
              "nothing was pacing the view that asked for a frame");

        presenter.record.requestFrameCallback();
    }

    // No pump in between, which is exactly what a host gives a plugin between
    // close() and dlclose(). The thread was joined before the view's destructor
    // returned, but the kernel lists a just-exited thread under /proc for a
    // moment longer, and a loaded machine stretches that moment.
    check(threadCountSettlesTo(idle),
          "a pacing thread outlived the last presenting view");
};

auto tSetContentViewTwice =
    test("EmbeddedView/setContentViewAgainMovesTheSurfacesContent") = []
{
    if (!x11ServerReachable())
        return;

    auto embedded = EmbeddedHost {400, 300};
    auto presenter = PresentingView {};
    auto second = RecordingView {};

    embedded.content.addSubview(presenter);
    presenter.setBounds({0.f, 0.f, 200.f, 150.f});

    check(embedded.pump.pumpUntil([&] { return presenter.available > 0; }));

    const auto host = embedded.host.geometry();

    embedded.view->setContentView(second);

    check(second.getBounds().w == (float) host.width,
          "the new content view was not laid out in the surface");
    check(second.getBounds().h == (float) host.height);

    // The tree that is no longer the surface's keeps no window of its own.
    check(presenter.lost == 1, "the old content view kept its child window");
    check(!presenter.hasSurface());

    embedded.pump.pumpFor(Time::MS {100});

    check(embedded.isUp(), "the surface itself went away with its content");
};

// Several surfaces in one window is the other kind of host the header names -
// and on X11 nothing about the first one makes the second any different.
auto tTwoSurfacesInOneHost = test("EmbeddedView/twoSurfacesShareOneHostWindow") = []
{
    if (!x11ServerReachable())
        return;

    auto embedded = EmbeddedHost {600, 400};
    auto secondContent = RecordingView {};

    auto* probe = embedded.connection.connection;
    const auto hostId = embedded.host.window;

    auto second = std::optional<EmbeddedView> {
        std::in_place, reinterpret_cast<void*>((uintptr_t) hostId)};

    second->setContentView(secondContent);

    const auto secondId = (xcb_window_t) (uintptr_t) second->getHandle();

    if (!checkArrived(secondId != XCB_NONE && secondId != embedded.id(),
                      "the second surface got no window of its own"))
        return;

    // Waited for, like every other question this file asks the server about a
    // window eacp's connection has only just created.
    check(
        embedded.pump.pumpUntil([&] { return parentOf(probe, secondId) == hostId; }),
        "the second surface is not a child of the same host window");

    embedded.host.resizeTo(500, 300);

    check(embedded.pump.pumpUntil(
              [&]
              {
                  const auto host = embedded.host.geometry();
                  const auto first = embedded.childGeometry();
                  const auto other = geometryOf(probe, secondId);

                  return host.found && host.width != 600 && first.width == host.width
                         && other.width == host.width && other.height == host.height;
              }),
          "both surfaces did not follow the host's resize");

    check(secondContent.getBounds().w == embedded.content.getBounds().w);

    // One going away is nothing to the other.
    second.reset();

    embedded.host.resizeTo(420, 260);

    check(embedded.pump.pumpUntil(
              [&]
              {
                  const auto host = embedded.host.geometry();
                  const auto first = embedded.childGeometry();

                  return host.found && host.width != 500 && first.found
                         && first.width == host.width && first.height == host.height;
              }),
          "the surviving surface stopped following the host");

    check(!geometryOf(probe, secondId).found,
          "the destroyed surface left its window behind");
};

auto tClickTakesTheKeyboardFocus =
    test("EmbeddedView/aClickTakesTheKeyboardFocus") = []
{
    if (!x11ServerReachable())
        return;

    auto embedded = EmbeddedHost {400, 300};

    if (!checkArrived(embedded.id() != XCB_NONE))
        return;

    auto* probe = embedded.connection.connection;
    auto input = FakeInput {probe};

    if (!canDriveTheSeat(probe, input))
        return;

    embedded.host.raise();
    embedded.pump.pumpFor(Time::MS {150});

    const auto origin = rootOriginOf(probe, embedded.id());

    input.moveTo(origin + Point {150.f, 80.f});
    embedded.pump.pumpFor(Time::MS {50});

    input.click(1);

    const auto child = embedded.id();

    check(embedded.pump.pumpUntil([&] { return focusedWindow(probe) == child; }),
          "a click inside the surface did not take the keyboard focus");

    if (!checkArrived(!embedded.content.downs.empty(),
                      "the click never reached the content view"))
        return;

    check(embedded.content.downs.front().pos.x == 150.f);
    check(embedded.content.downs.front().pos.y == 80.f);
};

auto tKeysReachTheContentView =
    test("EmbeddedView/aKeyPressReachesTheContentView") = []
{
    if (!x11ServerReachable())
        return;

    auto embedded = EmbeddedHost {400, 300};

    if (!checkArrived(embedded.id() != XCB_NONE))
        return;

    auto* probe = embedded.connection.connection;
    auto input = FakeInput {probe};

    if (!canDriveTheSeat(probe, input))
        return;

    embedded.host.raise();
    embedded.pump.pumpFor(Time::MS {150});

    // A host forwards nothing and gives nothing back: the click is the whole
    // of how an embedded surface gets the keyboard (plan.md D6).
    const auto origin = rootOriginOf(probe, embedded.id());

    input.moveTo(origin + Point {100.f, 60.f});
    embedded.pump.pumpFor(Time::MS {50});
    input.click(1);

    const auto child = embedded.id();

    if (!checkArrived(
            embedded.pump.pumpUntil([&] { return focusedWindow(probe) == child; }),
            "the surface never took the keyboard focus"))
        return;

    input.key(KEY_A, true);
    input.key(KEY_A, false);

    embedded.pump.pumpUntil([&] { return !embedded.content.keyUps.empty(); });

    if (!checkArrived(!embedded.content.keyDowns.empty(),
                      "no keyDown reached the content view"))
        return;

    const auto& down = embedded.content.keyDowns.front();

    check(down.keyCode == KeyCode::A);
    check(down.characters == "a");
    check(!down.isRepeat);

    check(!embedded.content.keyUps.empty());
};

// Nothing wraps this: the whole point is that eacp's loop never runs, and
// pumpEventLoop refuses re-entry, so a case inside Apps::run would pump
// nothing at all.
int main(int argc, char* argv[])
{
    // Defaulted, not forced: Scripts/with-xvfb has already set it, and a run
    // by hand on a Wayland desktop should still reach XWayland.
    ::setenv("EACP_WINDOW_SYSTEM", "x11", 0);

    Threads::attachCurrentThreadAsMain();

    return nano::run(argc, argv);
}
