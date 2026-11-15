#include "Common.h"

#include <eacp/Core/App/AppEnvironment.h>
#include <eacp/Core/Threads/EventLoop-Linux.h>
#include <eacp/Core/Utils/Environment.h>
#include <eacp/Graphics/Menu/LinuxMenuBar-Linux.h>
#include <eacp/Graphics/View/View-Linux.h>
#include <eacp/Graphics/Window/EmbeddedView.h>
#include <eacp/Graphics/Window/NativeChildSurface.h>

#include <xcb/xcb.h>

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <poll.h>
#include <string>
#include <unistd.h>
#include <vector>

// The Linux NativeChildSurface against a real server, with the test playing
// both sides of a plugin host: its own connection owns the host's window, an
// EmbeddedView or a toplevel of eacp's holds the surface, and a second
// connection of the test's own plays the plugin whose X11 window is parented
// into it. Everything asserted comes from the server.
//
// A plain main, as EmbeddedViewTests has: eacp is pumped through
// getEventLoopFd()/pumpEventLoop(), which a case inside Apps::run could not do.
// Every case self-skips without an X server unless EACP_REQUIRE_DISPLAY=1;
// Scripts/with-xvfb runs them for real.

using namespace nano;
using namespace eacp;
using namespace eacp::Graphics;

namespace NativeChildSurfaceTests
{
constexpr auto childTestTimeout = Time::MS {5000};
constexpr auto childPumpSlice = 8;

constexpr uint32_t xembedEmbeddedNotify = 0;
constexpr uint32_t xembedRequestFocus = 3;
constexpr uint32_t xembedFocusIn = 4;
constexpr uint32_t xembedMapped = 1;

bool x11ServerReachable()
{
    if (Apps::getAppEnvironment().headless)
        return false;

    if (getEnvValue("EACP_WINDOW_SYSTEM") != "x11")
        return false;

    return !getEnvValue("DISPLAY").empty();
}

struct Pump
{
    template <typename Predicate>
    bool until(Predicate ready, Time::MS timeout = childTestTimeout)
    {
        auto deadline = Time::Deadline {timeout};

        while (!ready())
        {
            if (deadline.expired())
                return ready();

            const auto remaining = (int) deadline.remaining().count;

            auto fds = pollfd {Threads::getEventLoopFd(), POLLIN, 0};
            ::poll(&fds, 1, std::min(remaining, childPumpSlice));

            Threads::pumpEventLoop();
        }

        return true;
    }

    void forDuration(Time::MS howLong)
    {
        until([] { return false; }, howLong);
    }
};

struct Connection
{
    Connection()
        : connection(xcb_connect(nullptr, nullptr))
    {
        if (xcb_connection_has_error(connection) != 0)
        {
            xcb_disconnect(connection);
            connection = nullptr;
        }
    }

    ~Connection()
    {
        if (connection != nullptr)
            xcb_disconnect(connection);
    }

    Connection(const Connection&) = delete;
    Connection& operator=(const Connection&) = delete;

    bool isValid() const { return connection != nullptr; }

    xcb_screen_t* screen() const
    {
        return xcb_setup_roots_iterator(xcb_get_setup(connection)).data;
    }

    xcb_atom_t intern(const char* name) const
    {
        auto* reply = xcb_intern_atom_reply(
            connection,
            xcb_intern_atom(connection, 0, (uint16_t) std::strlen(name), name),
            nullptr);

        if (reply == nullptr)
            return XCB_ATOM_NONE;

        const auto atom = reply->atom;
        std::free(reply);

        return atom;
    }

    xcb_connection_t* connection = nullptr;
};

struct Geometry
{
    bool found = false;
    int x = 0;
    int y = 0;
    int width = 0;
    int height = 0;
};

Geometry geometryOf(const Connection& connection, xcb_window_t window)
{
    auto geometry = Geometry {};

    if (window == XCB_NONE || !connection.isValid())
        return geometry;

    auto* xcb = connection.connection;
    auto* reply =
        xcb_get_geometry_reply(xcb, xcb_get_geometry(xcb, window), nullptr);

    if (reply == nullptr)
        return geometry;

    geometry = {true, reply->x, reply->y, reply->width, reply->height};
    std::free(reply);

    return geometry;
}

bool hasGeometry(const Geometry& geometry, int x, int y, int width, int height)
{
    return geometry.found && geometry.x == x && geometry.y == y
           && geometry.width == width && geometry.height == height;
}

xcb_window_t parentOf(const Connection& connection, xcb_window_t window)
{
    if (window == XCB_NONE || !connection.isValid())
        return XCB_NONE;

    auto* xcb = connection.connection;
    auto* reply = xcb_query_tree_reply(xcb, xcb_query_tree(xcb, window), nullptr);

    if (reply == nullptr)
        return XCB_NONE;

    const auto parent = reply->parent;
    std::free(reply);

    return parent;
}

// XCB_MAP_STATE_UNMAPPED, _UNVIEWABLE or _VIEWABLE, and nothing for a window
// that does not exist.
std::optional<uint8_t> mapStateOf(const Connection& connection, xcb_window_t window)
{
    if (window == XCB_NONE || !connection.isValid())
        return {};

    auto* xcb = connection.connection;
    auto* reply = xcb_get_window_attributes_reply(
        xcb, xcb_get_window_attributes(xcb, window), nullptr);

    if (reply == nullptr)
        return {};

    const auto state = reply->map_state;
    std::free(reply);

    return state;
}

bool isViewable(const Connection& connection, xcb_window_t window)
{
    return mapStateOf(connection, window) == XCB_MAP_STATE_VIEWABLE;
}

xcb_window_t focusedWindow(const Connection& connection)
{
    if (!connection.isValid())
        return XCB_NONE;

    auto* xcb = connection.connection;
    auto* reply = xcb_get_input_focus_reply(xcb, xcb_get_input_focus(xcb), nullptr);

    if (reply == nullptr)
        return XCB_NONE;

    const auto focus = reply->focus;
    std::free(reply);

    return focus;
}

// What a DAW owns: a window on a connection of its own, in the far corner
// where no other suite's window lands.
struct HostWindow
{
    HostWindow(Connection& connectionToUse, int width, int height)
        : connection(connectionToUse)
    {
        if (!connection.isValid())
            return;

        auto* screen = connection.screen();
        auto* xcb = connection.connection;

        window = xcb_generate_id(xcb);

        const auto x = std::max((int) screen->width_in_pixels - width - 40, 0);
        const auto y = std::max((int) screen->height_in_pixels - height - 40, 0);

        const uint32_t values[] = {screen->black_pixel,
                                   XCB_EVENT_MASK_STRUCTURE_NOTIFY};

        xcb_create_window(xcb,
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

        const uint32_t above = XCB_STACK_MODE_ABOVE;

        xcb_map_window(xcb, window);
        xcb_configure_window(xcb, window, XCB_CONFIG_WINDOW_STACK_MODE, &above);
        xcb_flush(xcb);
    }

    ~HostWindow() { destroyNow(); }

    void destroyNow()
    {
        if (window == XCB_NONE)
            return;

        xcb_destroy_window(connection.connection, window);
        xcb_flush(connection.connection);

        window = XCB_NONE;
    }

    bool isUp() const { return isViewable(connection, window); }

    Connection& connection;
    xcb_window_t window = XCB_NONE;
};

struct XembedMessage
{
    uint32_t opcode = 0;
    uint32_t detail = 0;
    uint32_t data1 = 0;
    uint32_t data2 = 0;
};

// The plugin: an X11 window on a client of its own, exactly as a VST3 editor
// built on any toolkit is.
struct ForeignClient
{
    explicit ForeignClient(xcb_window_t parent = XCB_NONE,
                           int width = 50,
                           int height = 40)
    {
        if (!connection.isValid())
            return;

        auto* xcb = connection.connection;
        auto* screen = connection.screen();

        xembed = connection.intern("_XEMBED");
        xembedInfo = connection.intern("_XEMBED_INFO");

        window = xcb_generate_id(xcb);

        const uint32_t values[] = {screen->white_pixel,
                                   XCB_EVENT_MASK_STRUCTURE_NOTIFY
                                       | XCB_EVENT_MASK_FOCUS_CHANGE};

        xcb_create_window(xcb,
                          XCB_COPY_FROM_PARENT,
                          window,
                          parent != XCB_NONE ? parent : screen->root,
                          0,
                          0,
                          (uint16_t) width,
                          (uint16_t) height,
                          0,
                          XCB_WINDOW_CLASS_INPUT_OUTPUT,
                          XCB_COPY_FROM_PARENT,
                          XCB_CW_BACK_PIXEL | XCB_CW_EVENT_MASK,
                          values);
        xcb_flush(xcb);
    }

    ~ForeignClient()
    {
        if (window != XCB_NONE && connection.isValid())
        {
            xcb_destroy_window(connection.connection, window);
            xcb_flush(connection.connection);
        }
    }

    void setXembedInfo(uint32_t flags)
    {
        const uint32_t info[] = {0, flags};

        xcb_change_property(connection.connection,
                            XCB_PROP_MODE_REPLACE,
                            window,
                            xembedInfo,
                            xembedInfo,
                            32,
                            2,
                            info);
        xcb_flush(connection.connection);
    }

    void reparentInto(xcb_window_t parent)
    {
        xcb_reparent_window(connection.connection, window, parent, 0, 0);
        xcb_flush(connection.connection);
    }

    void map()
    {
        xcb_map_window(connection.connection, window);
        xcb_flush(connection.connection);
    }

    void requestFocus(xcb_window_t embedder)
    {
        auto message = xcb_client_message_event_t {};
        message.response_type = XCB_CLIENT_MESSAGE;
        message.format = 32;
        message.window = embedder;
        message.type = xembed;
        message.data.data32[1] = xembedRequestFocus;

        xcb_send_event(connection.connection,
                       0,
                       embedder,
                       XCB_EVENT_MASK_NO_EVENT,
                       reinterpret_cast<const char*>(&message));
        xcb_flush(connection.connection);
    }

    void readEvents()
    {
        while (auto* event = xcb_poll_for_event(connection.connection))
        {
            const auto type = event->response_type & 0x7f;

            if (type == 0)
                ++errors;

            if (type == XCB_CLIENT_MESSAGE)
            {
                const auto* message =
                    reinterpret_cast<const xcb_client_message_event_t*>(event);

                if (message->type == xembed)
                    messages.push_back({message->data.data32[1],
                                        message->data.data32[2],
                                        message->data.data32[3],
                                        message->data.data32[4]});
            }

            std::free(event);
        }
    }

    std::optional<XembedMessage> received(uint32_t opcode)
    {
        readEvents();

        for (const auto& message: messages)
            if (message.opcode == opcode)
                return message;

        return {};
    }

    // Checked, so a window somebody else already destroyed shows as an error
    // here rather than as a stray event later.
    bool destroyCleanly()
    {
        auto* error = xcb_request_check(
            connection.connection,
            xcb_destroy_window_checked(connection.connection, window));

        window = XCB_NONE;

        if (error == nullptr)
            return true;

        std::free(error);
        return false;
    }

    Connection connection;
    xcb_window_t window = XCB_NONE;
    xcb_atom_t xembed = XCB_ATOM_NONE;
    xcb_atom_t xembedInfo = XCB_ATOM_NONE;
    std::vector<XembedMessage> messages;
    int errors = 0;
};

// The plugin editor's rectangle in a host's layout: a surface inside a frame
// view, inside an EmbeddedView of the host's window. The surface sits at
// (30, 40) in the window, which is what every geometry below is measured
// against.
struct Scene
{
    Scene()
        : host(connection, 600, 400)
    {
        pump.until([this] { return host.isUp(); });

        frame.setBounds({10.f, 10.f, 500.f, 300.f});
        surface.setBounds({20.f, 30.f, 200.f, 100.f});
        frame.addSubview(surface);
        content.addSubview(frame);

        embedded.emplace(reinterpret_cast<void*>((uintptr_t) host.window));
        embedded->setContentView(content);

        pump.until([this] { return isViewable(connection, embeddedWindow()); });
    }

    ~Scene() { embedded.reset(); }

    xcb_window_t embeddedWindow()
    {
        return embedded ? (xcb_window_t) (uintptr_t) embedded->getHandle()
                        : XCB_NONE;
    }

    xcb_window_t handle()
    {
        return (xcb_window_t) (uintptr_t) surface.getNativeParentHandle();
    }

    Geometry containerGeometry() { return geometryOf(connection, handle()); }

    // A client that maps itself and is inside, viewable.
    bool attach(ForeignClient& client)
    {
        client.reparentInto(handle());
        client.map();

        return pump.until([&] { return isViewable(connection, client.window); });
    }

    Connection connection;
    HostWindow host;
    Pump pump;

    View content;
    View frame;
    NativeChildSurface surface;

    std::optional<EmbeddedView> embedded;
};

// The protocol errors the backend logs go through LOG, so a case that must not
// provoke one reads them back out of a log file of its own.
struct LogCapture
{
    LogCapture()
        : path(std::filesystem::temp_directory_path()
               / ("eacp-native-child-" + std::to_string(::getpid()) + ".log"))
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

    bool sawProtocolError() const
    {
        return read().find("X11: protocol error") != std::string::npos;
    }

    // By request: CreateWindow is 1, DestroyWindow 4, ReparentWindow 7.
    bool sawErrorFromRequest(int major) const
    {
        return read().find("from request " + std::to_string(major) + ".")
               != std::string::npos;
    }

    std::filesystem::path path;
};

auto tAServerIsPresentWhenRequired =
    test("NativeChildSurface/aServerIsPresentWhenRequired") = []
{
    const auto reachable = x11ServerReachable();

    LOG("X11 server: ", reachable ? getEnvValue("DISPLAY") : std::string {"none"});

    if (getEnvValue("EACP_REQUIRE_DISPLAY") != "1")
        return;

    check(reachable,
          "EACP_REQUIRE_DISPLAY=1 but no X server was reached - every other "
          "case in this binary would have skipped and reported a pass");
};

auto tNoHandleOutsideAWindow = test("NativeChildSurface/noHandleOutsideAWindow") = []
{
    auto parent = View {};
    auto surface = NativeChildSurface {};

    surface.setBounds({0.f, 0.f, 100.f, 100.f});
    parent.addSubview(surface);

    check(surface.getNativeParentHandle() == nullptr);

    surface.setVisible(false);
    surface.refreshPlacement();

    check(surface.getNativeParentHandle() == nullptr);
};

auto tTheHandleIsAWindowAtTheViewsBounds =
    test("NativeChildSurface/theHandleIsAWindowAtTheViewsBounds") = []
{
    if (!x11ServerReachable())
        return;

    auto scene = Scene {};

    check(scene.handle() != XCB_NONE);
    check(scene.handle() == scene.handle(), "the handle changed between asks");
    check(parentOf(scene.connection, scene.handle()) == scene.embeddedWindow());
    check(hasGeometry(scene.containerGeometry(), 30, 40, 200, 100));
    check(scene.pump.until(
        [&] { return isViewable(scene.connection, scene.handle()); }));
};

auto tAReparentedClientFillsTheSurface =
    test("NativeChildSurface/aReparentedClientFillsTheSurface") = []
{
    if (!x11ServerReachable())
        return;

    auto scene = Scene {};
    auto client = ForeignClient {};

    check(scene.attach(client), "the client never became viewable inside");
    check(parentOf(scene.connection, client.window) == scene.handle());
    check(scene.pump.until(
        [&]
        {
            return hasGeometry(
                geometryOf(scene.connection, client.window), 0, 0, 200, 100);
        }));

    client.readEvents();
    check(client.errors == 0);
};

auto tAClientCreatedInsideFillsTheSurface =
    test("NativeChildSurface/aClientCreatedInsideFillsTheSurface") = []
{
    if (!x11ServerReachable())
        return;

    auto scene = Scene {};
    auto client = ForeignClient {scene.handle(), 64, 32};

    client.map();

    check(scene.pump.until([&]
                           { return isViewable(scene.connection, client.window); }));
    check(scene.pump.until(
        [&]
        {
            return hasGeometry(
                geometryOf(scene.connection, client.window), 0, 0, 200, 100);
        }));
};

auto tTheClientFollowsTheViewsBounds =
    test("NativeChildSurface/theClientFollowsTheViewsBounds") = []
{
    if (!x11ServerReachable())
        return;

    auto scene = Scene {};
    auto client = ForeignClient {};

    check(scene.attach(client));

    scene.surface.setBounds({50.f, 60.f, 320.f, 180.f});

    check(scene.pump.until(
        [&] { return hasGeometry(scene.containerGeometry(), 60, 70, 320, 180); }));
    check(scene.pump.until(
        [&]
        {
            return hasGeometry(
                geometryOf(scene.connection, client.window), 0, 0, 320, 180);
        }));

    // An ancestor moving moves the surface without resizing it, and nothing
    // has to call refreshPlacement for it.
    scene.frame.setBounds({100.f, 5.f, 500.f, 300.f});

    check(scene.pump.until(
        [&] { return hasGeometry(scene.containerGeometry(), 150, 65, 320, 180); }));
};

auto tTheEmbeddersScaleIsThePixels =
    test("NativeChildSurface/theEmbeddersScaleIsThePixels") = []
{
    if (!x11ServerReachable())
        return;

    auto scene = Scene {};
    auto client = ForeignClient {};

    check(scene.attach(client));

    scene.embedded->setPixelsPerPoint(2.f);

    check(scene.pump.until(
        [&] { return hasGeometry(scene.containerGeometry(), 60, 80, 400, 200); }));
    check(scene.pump.until(
        [&]
        {
            return hasGeometry(
                geometryOf(scene.connection, client.window), 0, 0, 400, 200);
        }));
};

auto tTheEmbeddersScaleIsReported =
    test("NativeChildSurface/theEmbeddersScaleIsReported") = []
{
    if (!x11ServerReachable())
        return;

    auto scene = Scene {};
    auto reported = std::vector<float> {};

    scene.surface.onBackingScaleChanged = [&](float scale)
    { reported.push_back(scale); };

    check(scene.surface.backingScale() == 1.f);

    scene.embedded->setPixelsPerPoint(2.f);

    check(scene.surface.backingScale() == 2.f);
    check(reported == std::vector<float> {2.f});

    // The same scale again is no change.
    scene.embedded->setPixelsPerPoint(2.f);
    scene.surface.refreshPlacement();

    check(reported.size() == 1);

    scene.embedded->setPixelsPerPoint(1.5f);

    check(scene.surface.backingScale() == 1.5f);
    check(reported == std::vector<float> {2.f, 1.5f});

    // Out of every window it is 1, and going back into one says so.
    scene.frame.removeSubview(scene.surface);

    check(scene.surface.backingScale() == 1.f);

    scene.frame.addSubview(scene.surface);

    check(reported == std::vector<float> {2.f, 1.5f, 1.5f});
    check(scene.surface.backingScale() == 1.5f);
};

auto tNoWindowIsScaleOne = test("NativeChildSurface/noWindowIsScaleOne") = []
{
    auto surface = NativeChildSurface {};
    auto calls = 0;

    surface.onBackingScaleChanged = [&](float) { ++calls; };
    surface.refreshPlacement();

    check(surface.backingScale() == 1.f);
    check(calls == 0);
};

std::string resourceDatabaseOf(const Connection& connection)
{
    auto* xcb = connection.connection;
    auto root = connection.screen()->root;

    auto* reply = xcb_get_property_reply(
        xcb,
        xcb_get_property(
            xcb, 0, root, XCB_ATOM_RESOURCE_MANAGER, XCB_ATOM_STRING, 0, 65536),
        nullptr);

    if (reply == nullptr)
        return {};

    auto database = std::string {(const char*) xcb_get_property_value(reply),
                                 (size_t) xcb_get_property_value_length(reply)};

    std::free(reply);

    return database;
}

void writeResourceDatabase(const Connection& connection, const std::string& database)
{
    xcb_change_property(connection.connection,
                        XCB_PROP_MODE_REPLACE,
                        connection.screen()->root,
                        XCB_ATOM_RESOURCE_MANAGER,
                        XCB_ATOM_STRING,
                        8,
                        (uint32_t) database.size(),
                        database.c_str());
    xcb_flush(connection.connection);
}

// A toplevel's scale is the display's Xft.dpi, which only a server of this
// suite's own may have rewritten: a desktop's resource database is the
// desktop's.
auto tAToplevelsDpiIsReported =
    test("NativeChildSurface/aToplevelsDpiIsReported") = []
{
    if (!x11ServerReachable() || getEnvValue("EACP_XVFB_OUTPUT").empty())
        return;

    auto connection = Connection {};
    auto pump = Pump {};

    auto content = View {};
    auto surface = NativeChildSurface {};
    auto reported = std::vector<float> {};

    surface.onBackingScaleChanged = [&](float scale) { reported.push_back(scale); };
    surface.setBounds({10.f, 20.f, 100.f, 50.f});
    content.addSubview(surface);

    auto options = WindowOptions {};
    options.width = 300;
    options.height = 200;
    options.title = "eacp native child surface scale";
    options.isPrimary = false;

    const auto original = resourceDatabaseOf(connection);

    {
        auto window = Window {options};
        window.setContentView(content);

        check(pump.until([&] { return window.isVisible(); }));

        const auto before = surface.backingScale();
        const auto handle =
            (xcb_window_t) (uintptr_t) surface.getNativeParentHandle();

        writeResourceDatabase(connection, "Xft.dpi:\t192\n");

        check(pump.until([&] { return surface.backingScale() == 2.f; }),
              "Xft.dpi 192 never reached the surface");
        check(!reported.empty() && reported.back() == 2.f);
        check(pump.until(
            [&]
            {
                return hasGeometry(geometryOf(connection, handle), 20, 40, 200, 100);
            }));

        writeResourceDatabase(connection, original);

        check(pump.until([&] { return surface.backingScale() == before; }));
        check(reported.back() == before);
    }

    content.removeSubview(surface);
};

auto tHidingUnmapsAndShowingMapsAgain =
    test("NativeChildSurface/hidingUnmapsAndShowingMapsAgain") = []
{
    if (!x11ServerReachable())
        return;

    auto scene = Scene {};
    auto client = ForeignClient {};

    check(scene.attach(client));

    const auto handle = scene.handle();

    scene.surface.setVisible(false);

    check(scene.pump.until(
        [&]
        {
            return mapStateOf(scene.connection, client.window)
                   == XCB_MAP_STATE_UNVIEWABLE;
        }));
    check(mapStateOf(scene.connection, handle) == XCB_MAP_STATE_UNMAPPED);

    scene.surface.setVisible(true);

    check(scene.pump.until([&]
                           { return isViewable(scene.connection, client.window); }));

    // A hidden ancestor hides it the same way.
    scene.frame.setVisible(false);

    check(scene.pump.until(
        [&]
        {
            return mapStateOf(scene.connection, client.window)
                   == XCB_MAP_STATE_UNVIEWABLE;
        }));

    scene.frame.setVisible(true);

    check(scene.pump.until([&]
                           { return isViewable(scene.connection, client.window); }));
    check(scene.handle() == handle, "hiding replaced the container");
};

auto tAnXembedClientIsMappedByItsFlag =
    test("NativeChildSurface/anXembedClientIsMappedByItsFlag") = []
{
    if (!x11ServerReachable())
        return;

    auto scene = Scene {};
    auto client = ForeignClient {};

    client.setXembedInfo(0);
    client.reparentInto(scene.handle());

    check(scene.pump.until(
              [&] { return client.received(xembedEmbeddedNotify).has_value(); }),
          "the client was never sent XEMBED_EMBEDDED_NOTIFY");

    const auto notify = *client.received(xembedEmbeddedNotify);

    check(notify.data1 == scene.handle(), "the notify named another embedder");
    check(notify.data2 == 0, "the notify offered a protocol version past 0");

    scene.pump.forDuration(Time::MS {100});
    check(mapStateOf(scene.connection, client.window) == XCB_MAP_STATE_UNMAPPED);

    client.setXembedInfo(xembedMapped);

    check(scene.pump.until([&]
                           { return isViewable(scene.connection, client.window); }),
          "XEMBED_MAPPED did not map the client");

    client.setXembedInfo(0);

    check(scene.pump.until(
              [&]
              {
                  return mapStateOf(scene.connection, client.window)
                         == XCB_MAP_STATE_UNMAPPED;
              }),
          "clearing XEMBED_MAPPED did not unmap the client");

    client.readEvents();
    check(client.errors == 0);
};

auto tAFocusRequestIsGranted = test("NativeChildSurface/aFocusRequestIsGranted") = []
{
    if (!x11ServerReachable())
        return;

    auto scene = Scene {};
    auto client = ForeignClient {};

    client.setXembedInfo(xembedMapped);
    client.reparentInto(scene.handle());

    check(scene.pump.until([&]
                           { return isViewable(scene.connection, client.window); }));

    client.requestFocus(scene.handle());

    check(
        scene.pump.until([&] { return client.received(xembedFocusIn).has_value(); }),
        "the client was never sent XEMBED_FOCUS_IN");
    check(scene.pump.until(
              [&] { return focusedWindow(scene.connection) == client.window; }),
          "the X focus never reached the client");
};

// The backstop a host should never need: the surface goes while the plugin is
// still attached, and the plugin's window must outlive it for the plugin to
// destroy later, or its own XDestroyWindow is a BadWindow, which Xlib's
// default handler turns into an exit.
auto tDestroyingItLetsTheClientGo =
    test("NativeChildSurface/destroyingItLetsTheClientGo") = []
{
    if (!x11ServerReachable())
        return;

    auto log = LogCapture {};
    auto client = ForeignClient {};

    {
        auto scene = Scene {};
        auto editor = std::optional<NativeChildSurface> {};

        editor.emplace();
        editor->setBounds({0.f, 0.f, 120.f, 80.f});
        scene.frame.addSubview(*editor);

        const auto handle =
            (xcb_window_t) (uintptr_t) editor->getNativeParentHandle();

        client.reparentInto(handle);
        client.map();

        check(scene.pump.until(
            [&] { return isViewable(scene.connection, client.window); }));

        editor.reset();

        check(scene.pump.until(
                  [&] { return !mapStateOf(scene.connection, handle).has_value(); }),
              "the container outlived the surface");

        check(parentOf(scene.connection, client.window)
              == scene.connection.screen()->root);
        check(mapStateOf(scene.connection, client.window) == XCB_MAP_STATE_UNMAPPED);
    }

    client.readEvents();

    check(client.errors == 0);
    check(client.destroyCleanly(), "the plugin's own window was destroyed for it");
    check(!log.sawProtocolError());
};

// The host closes the window the editor was in, with the plugin attached: the
// container leaves it rather than go down with it, and the same id goes into
// the next window it is put in.
auto tTheWindowGoingAwayLetsTheClientGo =
    test("NativeChildSurface/theWindowGoingAwayLetsTheClientGo") = []
{
    if (!x11ServerReachable())
        return;

    auto log = LogCapture {};
    auto scene = Scene {};
    auto client = ForeignClient {};

    check(scene.attach(client));

    const auto handle = scene.handle();

    scene.embedded.reset();

    scene.pump.forDuration(Time::MS {100});

    check(mapStateOf(scene.connection, client.window).has_value(),
          "the client went down with the window");
    check(parentOf(scene.connection, client.window) == handle);
    check(parentOf(scene.connection, handle) == scene.connection.screen()->root);
    check(mapStateOf(scene.connection, handle) == XCB_MAP_STATE_UNMAPPED);

    scene.embedded.emplace(reinterpret_cast<void*>((uintptr_t) scene.host.window));
    scene.embedded->setContentView(scene.content);

    check(scene.pump.until([&]
                           { return isViewable(scene.connection, client.window); }),
          "the client did not come back in the new window");
    check(scene.handle() == handle, "the surface's handle changed");
    check(parentOf(scene.connection, handle) == scene.embeddedWindow());

    client.readEvents();
    check(client.errors == 0);
    check(!log.sawProtocolError());
};

// The host's own window goes, which takes every inferior with it - ours and
// the plugin's - and nothing of ours may then ask about them again.
auto tTheHostDestroyingItsWindowIsClean =
    test("NativeChildSurface/theHostDestroyingItsWindowIsClean") = []
{
    if (!x11ServerReachable())
        return;

    auto scene = Scene {};
    auto client = ForeignClient {};

    check(scene.attach(client));

    const auto handle = scene.handle();
    auto log = LogCapture {};

    scene.host.destroyNow();

    check(scene.pump.until(
        [&] { return !mapStateOf(scene.connection, handle).has_value(); }));

    scene.pump.forDuration(Time::MS {100});

    check(scene.surface.getNativeParentHandle() == nullptr);

    scene.surface.setBounds({0.f, 0.f, 50.f, 50.f});
    scene.surface.setVisible(false);
    scene.surface.setVisible(true);
    scene.embedded.reset();

    scene.pump.forDuration(Time::MS {100});

    // Not any error at all: a request already on its way when another client
    // destroyed the window is the host's timing. Making, moving or destroying
    // a window inside one the server already reaped is ours.
    check(!log.sawErrorFromRequest(1), "a container was made in a dead window");
    check(!log.sawErrorFromRequest(4), "a dead container was destroyed again");
    check(!log.sawErrorFromRequest(7), "a dead container was reparented");

    client.window = XCB_NONE;
};

// A host built on eacp windows: the surface is a child of the toplevel itself.
auto tAToplevelWindowHoldsTheSurface =
    test("NativeChildSurface/aToplevelWindowHoldsTheSurface") = []
{
    if (!x11ServerReachable())
        return;

    auto connection = Connection {};
    auto pump = Pump {};

    auto content = View {};
    auto surface = NativeChildSurface {};

    surface.setBounds({16.f, 24.f, 240.f, 120.f});
    content.addSubview(surface);

    auto options = WindowOptions {};
    options.width = 400;
    options.height = 300;
    options.title = "eacp native child surface tests";
    options.isPrimary = false;

    auto client = ForeignClient {};

    {
        auto window = Window {options};
        window.setContentView(content);

        check(pump.until([&] { return window.isVisible(); }));

        const auto toplevel = (xcb_window_t) (uintptr_t) window.getHandle();
        const auto handle =
            (xcb_window_t) (uintptr_t) surface.getNativeParentHandle();

        check(handle != XCB_NONE);
        check(parentOf(connection, handle) == toplevel);

        client.reparentInto(handle);
        client.map();

        check(pump.until([&] { return isViewable(connection, client.window); }));
        check(pump.until(
            [&]
            {
                return geometryOf(connection, client.window).width
                       == geometryOf(connection, handle).width;
            }));
    }

    pump.forDuration(Time::MS {100});

    check(mapStateOf(connection, client.window).has_value(),
          "the client went down with the toplevel");

    content.removeSubview(surface);
};

// The toplevel's children, bottom of the stack first.
std::vector<xcb_window_t> stackOf(const Connection& connection, xcb_window_t window)
{
    auto* xcb = connection.connection;
    auto* reply = xcb_query_tree_reply(xcb, xcb_query_tree(xcb, window), nullptr);

    if (reply == nullptr)
        return {};

    auto* children = xcb_query_tree_children(reply);
    auto stack = std::vector<xcb_window_t>(
        children, children + xcb_query_tree_children_length(reply));

    std::free(reply);

    return stack;
}

// A menu bar whose view presents, as eacp-ui's does, so it has a child window
// of its own; the bar is drawn by nobody, only stacked.
struct PresentingMenuBar final : LinuxMenuBarPresenter
{
    PresentingMenuBar()
        : surface(requestViewSurface(view))
    {
    }

    View& getView() override { return view; }
    float getHeight() const override { return 24.f; }

    void layout(Rect windowBounds) override
    {
        view.setBounds({0.f, 0.f, windowBounds.w, 24.f});
    }

    bool isMenuOpen() const override { return false; }
    void dismissMenu() override {}
    bool keyPressed(const KeyEvent&) override { return false; }

    View view;
    ViewSurface& surface;
};

// A container made after the menu bar's surface would start above it and hide
// an open menu; it goes under the bar instead.
auto tAMenuBarStaysAboveTheSurface =
    test("NativeChildSurface/aMenuBarStaysAboveTheSurface") = []
{
    if (!x11ServerReachable())
        return;

    auto connection = Connection {};
    auto pump = Pump {};

    PresentingMenuBar* bar = nullptr;

    setLinuxMenuBarPresenterFactory(
        [&bar](const MenuBar&, Callback) -> std::unique_ptr<LinuxMenuBarPresenter>
        {
            auto made = std::make_unique<PresentingMenuBar>();
            bar = made.get();
            return made;
        });

    {
        auto content = View {};
        auto surface = NativeChildSurface {};

        surface.setBounds({16.f, 24.f, 240.f, 120.f});

        auto options = WindowOptions {};
        options.width = 400;
        options.height = 300;
        options.title = "eacp native child surface tests";
        options.isPrimary = false;

        auto window = Window {options};
        window.setContentView(content);

        auto file = Menu {"File"};
        file.add(MenuItem::withAction("Quit"));

        auto menus = MenuBar {};
        menus.add(std::move(file));
        setApplicationMenuBar(menus, window);

        check(bar != nullptr);
        check(pump.until(
            [&] { return window.isVisible() && bar->surface.handle.window != 0; }));

        // Only now, so its container is made after the bar's child window.
        content.addSubview(surface);

        const auto toplevel = (xcb_window_t) (uintptr_t) window.getHandle();
        const auto container =
            (xcb_window_t) (uintptr_t) surface.getNativeParentHandle();
        const auto barWindow = (xcb_window_t) bar->surface.handle.window;

        check(container != XCB_NONE);

        auto stacked = false;

        check(pump.until(
            [&]
            {
                auto stack = stackOf(connection, toplevel);
                auto barAt = std::ranges::find(stack, barWindow);
                auto containerAt = std::ranges::find(stack, container);

                stacked = barAt != stack.end() && containerAt != stack.end()
                          && barAt > containerAt;
                return stacked;
            }));

        check(stacked, "the container was left above the menu bar");

        content.removeSubview(surface);
    }

    setLinuxMenuBarPresenterFactory({});
};
} // namespace NativeChildSurfaceTests

int main(int argc, char* argv[])
{
    // Defaulted, not forced: Scripts/with-xvfb has already set it, and a run
    // by hand on a Wayland desktop should still reach XWayland.
    ::setenv("EACP_WINDOW_SYSTEM", "x11", 0);

    Threads::attachCurrentThreadAsMain();

    // Held for the whole run: an Xvfb whose last client disconnects resets,
    // and a connection eacp opens during the reset fails for the process.
    auto keepServerUp = std::optional<NativeChildSurfaceTests::Connection> {};

    if (NativeChildSurfaceTests::x11ServerReachable())
        keepServerUp.emplace();

    return nano::run(argc, argv);
}
