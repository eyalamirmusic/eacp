#include "X11Connection-Linux.h"

#include "../View/X11ViewSurface-Linux.h"
#include "X11Input-Linux.h"

#include <eacp/Core/App/AppEnvironment.h>
#include <eacp/Core/Threads/EventLoop-Linux.h>
#include <eacp/Core/Utils/Environment.h>

#include <xcb/randr.h>
#include <xcb/xfixes.h>
#include <xkbcommon/xkbcommon-x11.h>

#include <cstdlib>
#include <cstring>
#include <iterator>
#include <poll.h>

// Events are drained with xcb_poll_for_queued_event before every poll(2) as
// well as after one: Mesa's Vulkan WSI is a second reader of this same
// connection, and what it pulled off the socket is already in xcb's queue.

namespace eacp::Graphics
{
namespace
{
constexpr uint16_t x11RandrVersionMajor = 1;
constexpr uint16_t x11RandrVersionMinor = 5;

template <typename T>
const T& x11EventAs(const xcb_generic_event_t& event)
{
    return *reinterpret_cast<const T*>(&event);
}

// Zero for an event that names no window, and so goes nowhere.
xcb_window_t x11EventWindow(const xcb_generic_event_t& event)
{
    switch (event.response_type & ~0x80)
    {
        case XCB_EXPOSE:
            return x11EventAs<xcb_expose_event_t>(event).window;

        case XCB_CONFIGURE_NOTIFY:
            return x11EventAs<xcb_configure_notify_event_t>(event).window;

        case XCB_MAP_NOTIFY:
            return x11EventAs<xcb_map_notify_event_t>(event).window;

        case XCB_UNMAP_NOTIFY:
            return x11EventAs<xcb_unmap_notify_event_t>(event).window;

        // The window that moved and the window that died, not the one the
        // event was selected on: both are ours in the only case we act on.
        case XCB_REPARENT_NOTIFY:
            return x11EventAs<xcb_reparent_notify_event_t>(event).window;

        case XCB_DESTROY_NOTIFY:
            return x11EventAs<xcb_destroy_notify_event_t>(event).window;

        case XCB_CLIENT_MESSAGE:
            return x11EventAs<xcb_client_message_event_t>(event).window;

        case XCB_PROPERTY_NOTIFY:
            return x11EventAs<xcb_property_notify_event_t>(event).window;

        // The `event` field, not `child`: the window the event was selected
        // on is ours, the one under the pointer need not be.
        case XCB_BUTTON_PRESS:
        case XCB_BUTTON_RELEASE:
            return x11EventAs<xcb_button_press_event_t>(event).event;

        case XCB_MOTION_NOTIFY:
            return x11EventAs<xcb_motion_notify_event_t>(event).event;

        case XCB_KEY_PRESS:
        case XCB_KEY_RELEASE:
            return x11EventAs<xcb_key_press_event_t>(event).event;

        case XCB_ENTER_NOTIFY:
        case XCB_LEAVE_NOTIFY:
            return x11EventAs<xcb_enter_notify_event_t>(event).event;

        case XCB_FOCUS_IN:
        case XCB_FOCUS_OUT:
            return x11EventAs<xcb_focus_in_event_t>(event).event;

        case XCB_SELECTION_NOTIFY:
            return x11EventAs<xcb_selection_notify_event_t>(event).requestor;

        case XCB_SELECTION_REQUEST:
            return x11EventAs<xcb_selection_request_event_t>(event).owner;

        case XCB_SELECTION_CLEAR:
            return x11EventAs<xcb_selection_clear_event_t>(event).owner;

        default:
            return XCB_NONE;
    }
}

xcb_screen_t* x11ScreenOf(xcb_connection_t* connection, int number)
{
    auto iterator = xcb_setup_roots_iterator(xcb_get_setup(connection));

    for (auto remaining = number; remaining > 0 && iterator.rem > 0; --remaining)
        xcb_screen_next(&iterator);

    return iterator.data;
}

// Owned by the caller, as every xcb reply is.
xcb_randr_get_crtc_info_reply_t* x11PrimaryCrtcInfo(xcb_connection_t* connection,
                                                    xcb_window_t root)
{
    auto* primary = xcb_randr_get_output_primary_reply(
        connection, xcb_randr_get_output_primary(connection, root), nullptr);

    if (primary == nullptr)
        return nullptr;

    const auto output = primary->output;
    std::free(primary);

    if (output == XCB_NONE)
        return nullptr;

    auto* info = xcb_randr_get_output_info_reply(
        connection,
        xcb_randr_get_output_info(connection, output, XCB_CURRENT_TIME),
        nullptr);

    if (info == nullptr)
        return nullptr;

    const auto crtc = info->crtc;
    std::free(info);

    if (crtc == XCB_NONE)
        return nullptr;

    return xcb_randr_get_crtc_info_reply(
        connection,
        xcb_randr_get_crtc_info(connection, crtc, XCB_CURRENT_TIME),
        nullptr);
}

int x11RefreshMilliHz(xcb_connection_t* connection,
                      xcb_window_t root,
                      xcb_randr_mode_t mode)
{
    if (mode == XCB_NONE)
        return 0;

    auto* resources = xcb_randr_get_screen_resources_current_reply(
        connection,
        xcb_randr_get_screen_resources_current(connection, root),
        nullptr);

    if (resources == nullptr)
        return 0;

    auto refresh = 0;
    const auto* modes = xcb_randr_get_screen_resources_current_modes(resources);
    const auto count =
        xcb_randr_get_screen_resources_current_modes_length(resources);

    for (auto i = 0; i < count; ++i)
    {
        if (modes[i].id != mode)
            continue;

        // The dot clock is in hertz and overflows 32 bits once multiplied.
        const auto ticks = (uint64_t) modes[i].htotal * (uint64_t) modes[i].vtotal;

        if (ticks != 0)
            refresh = (int) ((uint64_t) modes[i].dot_clock * 1000 / ticks);

        break;
    }

    std::free(resources);
    return refresh;
}

// Whether the environment names a display to connect to. Asked before any
// connection is opened, so it is the environment and nothing more.
bool x11ServerIsReachable()
{
    return !getEnvValue("DISPLAY").empty() && !Apps::getAppEnvironment().headless;
}
} // namespace

X11WindowSurface::X11WindowSurface()
{
    viewSurfaces = makeX11ViewSurfaceBackend(*this);
}

void X11WindowSurface::setWindow(xcb_window_t window)
{
    auto* connection = x11Connection();

    if (window == XCB_NONE || connection == nullptr)
    {
        nativeSurface = {};
        return;
    }

    nativeSurface = {NativeSurfaceHandle::Kind::X11,
                     connection->getConnection(),
                     nullptr,
                     window};
}

void X11WindowSurface::handleEvent(const xcb_generic_event_t&) {}

X11Connection::X11Connection()
{
    auto* opened = xcb_connect(nullptr, &screenNumber);

    if (xcb_connection_has_error(opened) != 0)
    {
        xcb_disconnect(opened);

        LOG("X11: could not connect to the display named by DISPLAY. Windows "
            "will be created without a surface, as they are under "
            "EACP_HEADLESS.");
        return;
    }

    screen = x11ScreenOf(opened, screenNumber);

    // Every window starts from a screen's root, so a setup that names none is
    // as good as no server at all.
    if (screen == nullptr)
    {
        xcb_disconnect(opened);

        LOG("X11: the display named by DISPLAY has no screen. Windows will be "
            "created without a surface, as they are under EACP_HEADLESS.");
        return;
    }

    connection = opened;
    connected = true;

    internAtoms();
    setupXkb();
    setupRandr();
    setupXfixes();

    if (xcb_cursor_context_new(connection, screen, &cursors) != 0)
    {
        cursors = nullptr;
        LOG("X11: no cursor theme could be opened; the pointer keeps whatever "
            "shape the window manager gave it.");
    }

    input = std::make_unique<X11Input>(*this);

    openLoopSource();
}

X11Connection::~X11Connection()
{
    // Before the disconnect: what it holds was made from this connection.
    input.reset();

    closeLoopSource();

    if (cursors != nullptr)
        xcb_cursor_context_free(cursors);

    if (connection != nullptr)
        xcb_disconnect(connection);
}

void X11Connection::internAtoms()
{
    struct Request
    {
        const char* name;
        xcb_atom_t X11Atoms::* member;
    };

    static constexpr Request requests[] = {
        {"WM_PROTOCOLS", &X11Atoms::wmProtocols},
        {"WM_DELETE_WINDOW", &X11Atoms::wmDeleteWindow},
        {"WM_STATE", &X11Atoms::wmState},
        {"WM_CHANGE_STATE", &X11Atoms::wmChangeState},
        {"_NET_WM_NAME", &X11Atoms::netWmName},
        {"_NET_WM_PID", &X11Atoms::netWmPid},
        {"UTF8_STRING", &X11Atoms::utf8String},
        {"_NET_WM_STATE", &X11Atoms::netWmState},
        {"_NET_WM_STATE_ABOVE", &X11Atoms::netWmStateAbove},
        {"_NET_WM_STATE_MAXIMIZED_HORZ", &X11Atoms::netWmStateMaximizedHorz},
        {"_NET_WM_STATE_MAXIMIZED_VERT", &X11Atoms::netWmStateMaximizedVert},
        {"_NET_WM_STATE_FULLSCREEN", &X11Atoms::netWmStateFullscreen},
        {"_NET_WM_STATE_HIDDEN", &X11Atoms::netWmStateHidden},
        {"_NET_ACTIVE_WINDOW", &X11Atoms::netActiveWindow},
        {"_MOTIF_WM_HINTS", &X11Atoms::motifWmHints},
        {"CLIPBOARD", &X11Atoms::clipboard},
        {"TARGETS", &X11Atoms::targets},
        {"INCR", &X11Atoms::incr},
        {"_NET_WM_WINDOW_TYPE", &X11Atoms::netWmWindowType},
        {"_NET_WM_WINDOW_TYPE_NORMAL", &X11Atoms::netWmWindowTypeNormal},
    };

    constexpr auto count = std::size(requests);

    xcb_intern_atom_cookie_t cookies[count] = {};

    for (auto i = size_t {0}; i < count; ++i)
        cookies[i] = xcb_intern_atom(connection,
                                     0,
                                     (uint16_t) std::strlen(requests[i].name),
                                     requests[i].name);

    for (auto i = size_t {0}; i < count; ++i)
    {
        if (auto* reply = xcb_intern_atom_reply(connection, cookies[i], nullptr))
        {
            atoms.*(requests[i].member) = reply->atom;
            std::free(reply);
        }
    }
}

// The event base comes from the setup rather than from xcb_get_extension_data
// because xcb/xkb.h names a struct field `explicit`, which C++ will not take.
void X11Connection::setupXkb()
{
    auto eventBase = uint8_t {0};

    if (xkb_x11_setup_xkb_extension(connection,
                                    XKB_X11_MIN_MAJOR_XKB_VERSION,
                                    XKB_X11_MIN_MINOR_XKB_VERSION,
                                    XKB_X11_SETUP_XKB_EXTENSION_NO_FLAGS,
                                    nullptr,
                                    nullptr,
                                    &eventBase,
                                    nullptr)
        == 0)
    {
        LOG("X11: the server has no XKB extension, so keyboard input will not "
            "be translated.");
        return;
    }

    xkbEventBase = eventBase;
    keyboardDeviceId = xkb_x11_get_core_keyboard_device_id(connection);
}

// GetOutputPrimary is a 1.3 request: a server told nothing about the version
// answers as 1.0 and rejects it.
void X11Connection::setupRandr()
{
    const auto* extension = xcb_get_extension_data(connection, &xcb_randr_id);

    if (extension == nullptr || extension->present == 0)
        return;

    auto* reply = xcb_randr_query_version_reply(
        connection,
        xcb_randr_query_version(
            connection, x11RandrVersionMajor, x11RandrVersionMinor),
        nullptr);

    if (reply == nullptr)
        return;

    randrAvailable = reply->major_version > 1 || reply->minor_version >= 3;
    std::free(reply);
}

// Nothing here needs XFixes yet; hiding the cursor for a mouse lock does, and
// the version has to be negotiated before any of its requests is sent.
void X11Connection::setupXfixes()
{
    const auto* extension = xcb_get_extension_data(connection, &xcb_xfixes_id);

    if (extension == nullptr || extension->present == 0)
        return;

    auto* reply = xcb_xfixes_query_version_reply(
        connection,
        xcb_xfixes_query_version(
            connection, XCB_XFIXES_MAJOR_VERSION, XCB_XFIXES_MINOR_VERSION),
        nullptr);

    std::free(reply);
}

void X11Connection::openLoopSource()
{
    loopFd = xcb_get_file_descriptor(connection);

    Threads::addLoopSource(
        loopFd, POLLIN, [this] { readAndDispatch(); }, [this] { prepareForPoll(); });
}

void X11Connection::closeLoopSource()
{
    if (loopFd >= 0)
    {
        Threads::removeLoopSource(loopFd);
        loopFd = -1;
    }
}

// Only any use immediately before poll(): the drain takes what another reader
// queued, the flush sends the requests still in xcb's buffer.
void X11Connection::prepareForPoll()
{
    if (!isConnected())
        return;

    while (auto* event = xcb_poll_for_queued_event(connection))
    {
        dispatch(*event);
        std::free(event);
    }

    flush();
}

void X11Connection::readAndDispatch()
{
    if (!isConnected())
        return;

    while (auto* event = xcb_poll_for_event(connection))
    {
        dispatch(*event);
        std::free(event);
    }

    checkForConnectionLoss();
}

// An X11 error is asynchronous: it names the request that failed and nothing
// else ever mentions it again, so a dropped one is a request that silently did
// nothing. Logged rather than fatal, exactly as a window manager treats them.
void X11Connection::reportError(const xcb_generic_error_t& error)
{
    LOG("X11: protocol error ",
        (int) error.error_code,
        " from request ",
        (int) error.major_code,
        ".",
        (int) error.minor_code,
        " on resource ",
        (unsigned) error.resource_id);
}

void X11Connection::dispatch(const xcb_generic_event_t& event)
{
    if ((event.response_type & 0x7f) == 0)
    {
        reportError(x11EventAs<xcb_generic_error_t>(event));
        return;
    }

    if (xkbEventBase != 0 && (event.response_type & ~0x80) == xkbEventBase)
    {
        onXkbEvent(event);
        return;
    }

    const auto window = x11EventWindow(event);

    if (window == XCB_NONE)
        return;

    // Passed through as it came: a view's child window is the native's, and
    // only the native knows what to make of an event on one.
    if (auto target = findWindow(window); target.windowSurface != nullptr)
        target.windowSurface->handleEvent(event);
}

void X11Connection::checkForConnectionLoss()
{
    if (connected && xcb_connection_has_error(connection) != 0)
        connectionLost();
}

// Every id the server owned is gone with it, and the process goes on with the
// windows it has left surfaceless - the state a build with no display is in
// from the start. The connection itself is not disconnected until the
// destructor: things made from it are still being torn down.
void X11Connection::connectionLost()
{
    if (!connected)
        return;

    LOG("X11: the connection to the display server was lost. Windows are now "
        "surfaceless, as they are under EACP_HEADLESS.");

    connected = false;

    closeLoopSource();

    // Snapshotted: every one of these unregisters the windows it made.
    auto lost = Vector<X11WindowSurface*> {};

    for (const auto& target: windows)
        if (target.windowSurface != nullptr && !lost.contains(target.windowSurface))
            lost.add(target.windowSurface);

    for (auto* surface: lost)
        surface->onConnectionLost();

    windows.clear();
}

void X11Connection::flush()
{
    if (!isConnected())
        return;

    xcb_flush(connection);
    checkForConnectionLoss();
}

const std::optional<X11OutputInfo>& X11Connection::getPrimaryOutput() const
{
    if (primaryOutput || !isConnected() || screen == nullptr)
        return primaryOutput;

    // No RandR, or a server with no primary output named (Xvfb has none): the
    // root window is the one output there is.
    auto output = X11OutputInfo {{0.f,
                                  0.f,
                                  (float) screen->width_in_pixels,
                                  (float) screen->height_in_pixels},
                                 0};

    if (auto* crtc =
            randrAvailable ? x11PrimaryCrtcInfo(connection, screen->root) : nullptr)
    {
        if (crtc->width > 0 && crtc->height > 0)
        {
            output.frame = {(float) crtc->x,
                            (float) crtc->y,
                            (float) crtc->width,
                            (float) crtc->height};
            output.refreshMilliHz =
                x11RefreshMilliHz(connection, screen->root, crtc->mode);
        }

        std::free(crtc);
    }

    primaryOutput = output;

    return primaryOutput;
}

void X11Connection::registerWindow(const X11WindowTarget& target)
{
    auto window = target.window;

    windows.removeIndexesMatching([window](const X11WindowTarget& existing)
                                  { return existing.window == window; });

    windows.add(target);
}

void X11Connection::unregisterWindow(xcb_window_t window)
{
    windows.removeIndexesMatching([window](const X11WindowTarget& target)
                                  { return target.window == window; });
}

X11WindowTarget X11Connection::findWindow(xcb_window_t window) const
{
    for (const auto& target: windows)
        if (target.window == window)
            return target;

    return {};
}

// Deliberately leaked, as waylandDisplay() is: the destructor would deregister
// from an event loop that may already be gone at static teardown.
X11Connection* x11Connection()
{
    static auto* instance = []() -> X11Connection*
    {
        if (!x11ServerIsReachable())
            return nullptr;

        auto* opened = new X11Connection();

        if (!opened->isValid())
        {
            delete opened;
            return nullptr;
        }

        return opened;
    }();

    return instance;
}
} // namespace eacp::Graphics
