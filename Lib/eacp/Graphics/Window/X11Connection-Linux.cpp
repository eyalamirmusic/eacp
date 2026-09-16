#include "X11Connection-Linux.h"

#include "../View/X11ViewSurface-Linux.h"
#include "X11Clipboard-Linux.h"
#include "X11Input-Linux.h"

#include <eacp/Core/App/AppEnvironment.h>
#include <eacp/Core/Threads/EventLoop-Linux.h>
#include <eacp/Core/Utils/Environment.h>

#include <xcb/randr.h>
#include <xcb/xfixes.h>
#include <xcb/xinput.h>
#include <xkbcommon/xkbcommon-x11.h>

#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <iterator>
#include <poll.h>
#include <string>
#include <string_view>

// Events are drained with xcb_poll_for_queued_event before every poll(2) as
// well as after one: Mesa's Vulkan WSI is a second reader of this same
// connection, and what it pulled off the socket is already in xcb's queue.

namespace eacp::Graphics
{
namespace
{
constexpr uint16_t x11RandrVersionMajor = 1;
constexpr uint16_t x11RandrVersionMinor = 5;

// 2.1 is where a scroll valuator replaces buttons 4-7; raw events are 2.0's,
// and a server with one but not the other is old enough that keeping the two
// halves apart would buy nothing.
constexpr uint16_t x11XinputVersionMajor = 2;
constexpr uint16_t x11XinputVersionMinor = 2;
constexpr uint16_t x11XinputLeastMinor = 1;

// The pointer events of a window of ours, which is every core pointer event
// with a valuator on it. Keys and focus stay core: XI2 adds nothing to either,
// and a server that suppressed the core ones would take the keymap with them.
constexpr uint32_t x11XinputPointerMask =
    XCB_INPUT_XI_EVENT_MASK_BUTTON_PRESS | XCB_INPUT_XI_EVENT_MASK_BUTTON_RELEASE
    | XCB_INPUT_XI_EVENT_MASK_MOTION | XCB_INPUT_XI_EVENT_MASK_ENTER
    | XCB_INPUT_XI_EVENT_MASK_LEAVE;

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

// The root window as the server has it now, which is not what the setup said
// once a RandR screen change has resized it: xcb reads the setup at connect
// and never revises it.
Rect x11RootFrame(xcb_connection_t* connection, const xcb_screen_t& screen)
{
    auto frame = Rect {
        0.f, 0.f, (float) screen.width_in_pixels, (float) screen.height_in_pixels};

    auto* reply = xcb_get_geometry_reply(
        connection, xcb_get_geometry(connection, screen.root), nullptr);

    if (reply == nullptr)
        return frame;

    if (reply->width > 0 && reply->height > 0)
        frame = {0.f, 0.f, (float) reply->width, (float) reply->height};

    std::free(reply);

    return frame;
}

// The xrdb database, as xrdb itself and every toolkit read it: one STRING
// property on the root, whose lines are `key:<whitespace>value`. Requested in
// one go and again with whatever the first reply said was left, so a database
// bigger than the guess is still read whole.
std::string x11ResourceDatabase(xcb_connection_t* connection, xcb_window_t root)
{
    constexpr uint32_t initialWords = 4096;

    auto read = [connection, root](uint32_t words) -> xcb_get_property_reply_t*
    {
        return xcb_get_property_reply(connection,
                                      xcb_get_property(connection,
                                                       0,
                                                       root,
                                                       XCB_ATOM_RESOURCE_MANAGER,
                                                       XCB_ATOM_STRING,
                                                       0,
                                                       words),
                                      nullptr);
    };

    auto* reply = read(initialWords);

    if (reply == nullptr)
        return {};

    if (reply->bytes_after > 0)
    {
        const auto needed = initialWords + (reply->bytes_after + 3) / 4;
        std::free(reply);

        reply = read(needed);

        if (reply == nullptr)
            return {};
    }

    auto database = std::string {(const char*) xcb_get_property_value(reply),
                                 (size_t) xcb_get_property_value_length(reply)};

    std::free(reply);

    return database;
}

// The value of one key, and nothing where the database does not name it. Only
// a whole line counts, so Xft.dpi is not found inside somebody else's
// XTerm*Xft.dpi.
std::string x11ResourceValue(std::string_view database, std::string_view key)
{
    for (auto rest = database; !rest.empty();)
    {
        const auto end = rest.find('\n');
        const auto line = rest.substr(0, end);

        rest = end == std::string_view::npos ? std::string_view {}
                                             : rest.substr(end + 1);

        const auto colon = line.find(':');

        if (colon == std::string_view::npos || line.substr(0, colon) != key)
            continue;

        auto value = line.substr(colon + 1);

        while (!value.empty() && (value.front() == ' ' || value.front() == '\t'))
            value.remove_prefix(1);

        while (
            !value.empty()
            && (value.back() == ' ' || value.back() == '\t' || value.back() == '\r'))
            value.remove_suffix(1);

        return std::string {value};
    }

    return {};
}

// 96 dpi is one pixel per point by definition, so a desktop at 200% writes 192
// and one at 150% writes 144. Anything outside what a panel could plausibly
// be is a database somebody else's program wrote badly, and 1 is safer than
// half a window.
float x11ScaleFromResources(std::string_view database)
{
    constexpr auto pointsPerInch = 96.f;
    constexpr auto lowestDpi = 24.f;
    constexpr auto highestDpi = 960.f;

    const auto dpi = x11ResourceValue(database, "Xft.dpi");

    if (dpi.empty())
        return 1.f;

    auto* end = (char*) nullptr;
    const auto value = std::strtof(dpi.c_str(), &end);

    if (end == dpi.c_str() || value < lowestDpi || value > highestDpi)
        return 1.f;

    return value / pointsPerInch;
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

void X11WindowSurface::scaleChanged(float) {}

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
    setupXinput();
    setupRandr();
    setupXfixes();
    setupRoot();
    openCursorContext();

    input = std::make_unique<X11Input>(*this);
    clipboard = std::make_unique<X11Clipboard>(*this);

    openLoopSource();
}

X11Connection::~X11Connection()
{
    // Before the disconnect: what they hold was made from this connection.
    clipboard.reset();
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
        {"TEXT", &X11Atoms::text},
        {"text/plain", &X11Atoms::textPlain},
        {"text/plain;charset=utf-8", &X11Atoms::textPlainUtf8},
        {"text/uri-list", &X11Atoms::textUriList},
        {"EACP_SELECTION", &X11Atoms::eacpSelection},
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

// The version has to be negotiated before any other XI2 request, and what the
// server answers is what it will speak from here: asking for 2.2 and being
// told 2.0 is a server with no scroll valuators, which is the core pointer
// path and nothing else.
void X11Connection::setupXinput()
{
    if (getEnvValue("EACP_X11_NO_XI2") == "1")
    {
        LOG("X11: EACP_X11_NO_XI2 is set, so the pointer is the core one: "
            "scrolling arrives as buttons 4-7 and a mouse lock measures "
            "itself from the warps that recentre it.");
        return;
    }

    const auto* extension = xcb_get_extension_data(connection, &xcb_input_id);

    if (extension == nullptr || extension->present == 0)
        return;

    auto* reply = xcb_input_xi_query_version_reply(
        connection,
        xcb_input_xi_query_version(
            connection, x11XinputVersionMajor, x11XinputVersionMinor),
        nullptr);

    if (reply == nullptr)
        return;

    const auto usable = reply->major_version > x11XinputVersionMajor
                        || (reply->major_version == x11XinputVersionMajor
                            && reply->minor_version >= x11XinputLeastMinor);

    std::free(reply);

    if (usable)
        xinputOpcode = extension->major_opcode;
}

uint32_t X11Connection::getWindowEventMask() const
{
    return isXinputAvailable() ? x11WindowEventMask & ~x11PointerEventMask
                               : x11WindowEventMask;
}

void X11Connection::selectPointerEvents(xcb_window_t window)
{
    selectXinputEvents(window, XCB_INPUT_DEVICE_ALL_MASTER, x11XinputPointerMask);
}

// One device spec and one mask word, which is all XI2's event numbers need:
// the request is a header followed by the mask, so the two are laid out
// together rather than sent as a pointer to each.
void X11Connection::selectXinputEvents(xcb_window_t window,
                                       uint16_t device,
                                       uint32_t mask)
{
    if (!isXinputAvailable() || !isConnected() || window == XCB_NONE)
        return;

    struct Selection
    {
        xcb_input_event_mask_t header;
        uint32_t mask;
    };

    auto selection = Selection {};
    selection.header.deviceid = device;
    selection.header.mask_len = 1;
    selection.mask = mask;

    xcb_input_xi_select_events(connection, window, 1, &selection.header);
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

    if (!randrAvailable)
        return;

    randrEventBase = extension->first_event;

    // A screen change is a resized root, a crtc change a mode or a position,
    // an output change a monitor plugged in: the first two move the frame the
    // pacer and Display report, and the third decides which output is primary.
    xcb_randr_select_input(connection,
                           screen->root,
                           XCB_RANDR_NOTIFY_MASK_SCREEN_CHANGE
                               | XCB_RANDR_NOTIFY_MASK_CRTC_CHANGE
                               | XCB_RANDR_NOTIFY_MASK_OUTPUT_CHANGE);
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

// A PropertyNotify on the root is how a session says the resource database
// changed, which is how it says the scale or the cursor theme did. The mask is
// per client, so selecting it takes nothing away from the window manager or
// from anybody else watching the same root.
void X11Connection::setupRoot()
{
    const uint32_t mask = XCB_EVENT_MASK_PROPERTY_CHANGE;

    xcb_change_window_attributes(connection, screen->root, XCB_CW_EVENT_MASK, &mask);

    scale = x11ScaleFromResources(x11ResourceDatabase(connection, screen->root));
}

// xcb-cursor reads the theme, its size and Xft.dpi out of the resource
// database once, here, and never looks again - so a database that changes is a
// context that has to be built anew.
void X11Connection::openCursorContext()
{
    if (xcb_cursor_context_new(connection, screen, &cursors) != 0)
    {
        cursors = nullptr;
        LOG("X11: no cursor theme could be opened; the pointer keeps whatever "
            "shape the window manager gave it.");
    }
}

void X11Connection::resourcesChanged()
{
    const auto database = x11ResourceDatabase(connection, screen->root);

    scaleChanged(x11ScaleFromResources(database));
    cursorThemeChanged();
}

void X11Connection::scaleChanged(float newScale)
{
    if (newScale <= 0.f || newScale == scale)
        return;

    scale = newScale;

    // Snapshotted and deduplicated: a window is in the map once per child
    // window a presenting view gave it.
    auto surfaces = Vector<X11WindowSurface*> {};

    for (const auto& target: windows)
        if (target.windowSurface != nullptr
            && !surfaces.contains(target.windowSurface))
            surfaces.add(target.windowSurface);

    for (auto* surface: surfaces)
        surface->scaleChanged(scale);
}

// The cursors already loaded came out of the old context and out of whatever
// the database said then, so they go with it; the seat reloads the shape the
// pointer is wearing from the new one.
void X11Connection::cursorThemeChanged()
{
    if (cursors != nullptr)
        xcb_cursor_context_free(cursors);

    cursors = nullptr;

    openCursorContext();

    if (input != nullptr)
        input->cursorThemeChanged();
}

void X11Connection::outputChanged()
{
    primaryOutput.reset();

    // Read back here rather than left to the next question: the pacer is the
    // one thing that has to be told rather than asked.
    getPrimaryOutput();

    x11FramePacerRateChanged();
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

    drainQueuedEvents();
    flush();
}

// What another reader of this socket - Mesa's WSI - already took off it and
// left in xcb's queue. Reads nothing itself.
void X11Connection::drainQueuedEvents()
{
    while (auto* event = xcb_poll_for_queued_event(connection))
    {
        dispatch(*event);
        std::free(event);
    }
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

    if (dispatchXinput(event))
        return;

    // Before the window map, and taking nothing from it: what the clipboard
    // answers to is its own never-mapped window, which is no toplevel of ours.
    if (clipboard != nullptr && clipboard->handleEvent(event))
        return;

    // RandR names a screen rather than a window of ours, and both of its
    // events - the screen's size and one output's mode or position - mean the
    // same thing here: read the output again.
    if (randrEventBase != 0)
    {
        const auto randr = (event.response_type & ~0x80) - randrEventBase;

        if (randr == XCB_RANDR_SCREEN_CHANGE_NOTIFY || randr == XCB_RANDR_NOTIFY)
        {
            outputChanged();
            return;
        }
    }

    const auto window = x11EventWindow(event);

    if (window == XCB_NONE)
        return;

    // The root is nobody's window: the resource database that names the scale
    // and the cursor theme is a property on it, and nothing else we hear about
    // it is ours.
    if (screen != nullptr && window == screen->root
        && (event.response_type & ~0x80) == XCB_PROPERTY_NOTIFY)
    {
        if (x11EventAs<xcb_property_notify_event_t>(event).atom
            == XCB_ATOM_RESOURCE_MANAGER)
            resourcesChanged();

        return;
    }

    // Passed through as it came: a view's child window is the native's, and
    // only the native knows what to make of an event on one.
    if (auto target = findWindow(window); target.windowSurface != nullptr)
        target.windowSurface->handleEvent(event);

    if (!watchers.empty())
        dispatchToWatchers(event, window);
}

// XI2 speaks entirely in GenericEvents, and one of them names no window at all:
// raw motion is selected on the root and delivered whatever has the pointer
// grabbed. So the whole extension goes to the seat, which is the only thing
// here that knows what a valuator means, and the window lookup a device event
// needs happens there.
bool X11Connection::dispatchXinput(const xcb_generic_event_t& event)
{
    if (xinputOpcode == 0 || (event.response_type & ~0x80) != XCB_GE_GENERIC)
        return false;

    if (x11EventAs<xcb_ge_generic_event_t>(event).extension != xinputOpcode)
        return false;

    if (input != nullptr)
        input->xinputEvent(event);

    return true;
}

// After the window's own target, and as well as it: a window of this copy's
// that an EmbeddedView was parented onto is both, and each has its own reason
// to hear the event. Over a copy of the list, because a watcher is allowed to
// take itself out from inside its own handler - and rechecked, because it may
// have taken another one out with it.
void X11Connection::dispatchToWatchers(const xcb_generic_event_t& event,
                                       xcb_window_t window)
{
    for (auto watching = watchers; const auto& watcher: watching)
        if (watcher.window == window && watchers.contains(watcher))
            watcher.windowSurface->handleEvent(event);
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

    if (clipboard != nullptr)
        clipboard->connectionLost();

    // Snapshotted: every one of these unregisters the windows it made.
    auto lost = Vector<X11WindowSurface*> {};

    for (const auto& target: windows)
        if (target.windowSurface != nullptr && !lost.contains(target.windowSurface))
            lost.add(target.windowSurface);

    for (auto* surface: lost)
        surface->onConnectionLost();

    windows.clear();
    watchers.clear();
}

void X11Connection::flush()
{
    if (!isConnected())
        return;

    xcb_flush(connection);
    checkForConnectionLoss();
}

// The loop source's two halves, run by hand: nothing is filtered out, so a
// window whose ConfigureNotify lands beside the clipboard's SelectionNotify
// hears about it here exactly as it would have a moment later.
bool X11Connection::dispatchUntil(const std::function<bool()>& satisfied,
                                  Time::Deadline deadline)
{
    while (isConnected())
    {
        drainQueuedEvents();

        if (satisfied())
            return true;

        flush();

        if (!isConnected() || deadline.expired())
            break;

        auto watched = pollfd {xcb_get_file_descriptor(connection), POLLIN, 0};
        const auto left =
            (int) std::clamp<int64_t>(deadline.remaining().count, 0, 1000);

        if (::poll(&watched, 1, left) < 0 && errno != EINTR)
            break;

        readAndDispatch();
    }

    return satisfied();
}

const std::optional<X11OutputInfo>& X11Connection::getPrimaryOutput() const
{
    if (primaryOutput || !isConnected() || screen == nullptr)
        return primaryOutput;

    // No RandR, or a server with no primary output named (Xvfb has none): the
    // root window is the one output there is.
    auto output = X11OutputInfo {x11RootFrame(connection, *screen), 0};

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

void X11Connection::watchForeignWindow(const X11WindowTarget& watcher)
{
    unwatchForeignWindow(*watcher.windowSurface);

    watchers.add(watcher);
}

void X11Connection::unwatchForeignWindow(const X11WindowSurface& surface)
{
    watchers.removeIndexesMatching([&surface](const X11WindowTarget& watcher)
                                   { return watcher.windowSurface == &surface; });
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
