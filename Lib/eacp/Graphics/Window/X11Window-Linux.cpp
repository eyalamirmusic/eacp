#include "X11Window-Linux.h"

#include "X11Connection-Linux.h"
#include "X11Input-Linux.h"

#include <xcb/xcb_icccm.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <optional>
#include <string>
#include <unistd.h>

// File-scope names carry an x11/X11 prefix: this is one unity TU under
// EACP_CI_BUILD.

namespace eacp::Graphics
{
namespace
{
// Everything the toplevel is ever told about, structure and seat alike. A
// view's child window selects Exposure and nothing else, so the pointer and
// key events over one propagate up to here already in the toplevel's points.
constexpr uint32_t x11ToplevelEventMask =
    XCB_EVENT_MASK_STRUCTURE_NOTIFY | XCB_EVENT_MASK_EXPOSURE
    | XCB_EVENT_MASK_FOCUS_CHANGE | XCB_EVENT_MASK_PROPERTY_CHANGE
    | XCB_EVENT_MASK_KEY_PRESS | XCB_EVENT_MASK_KEY_RELEASE
    | XCB_EVENT_MASK_BUTTON_PRESS | XCB_EVENT_MASK_BUTTON_RELEASE
    | XCB_EVENT_MASK_POINTER_MOTION | XCB_EVENT_MASK_ENTER_WINDOW
    | XCB_EVENT_MASK_LEAVE_WINDOW;

constexpr uint32_t x11NetWmStateToggle = 2;

// _NET_WM_STATE's source indication: 1 is an ordinary application, which is
// what a window manager expects of a request it should honour.
constexpr uint32_t x11ClientSourceApplication = 1;

// An aspect ratio is a pair of integers to the server; three decimal places is
// far past what any window shape is expressed in.
constexpr float x11AspectPrecision = 1000.f;

// The Motif hints are a de-facto standard with no header of their own: five
// 32-bit words, of which only the first three are ever read.
struct X11MotifHints
{
    uint32_t flags = 0;
    uint32_t functions = 0;
    uint32_t decorations = 0;
    int32_t inputMode = 0;
    uint32_t status = 0;
};

constexpr uint32_t x11MotifHintsFunctions = 1u << 0;
constexpr uint32_t x11MotifHintsDecorations = 1u << 1;

constexpr uint32_t x11MotifFuncResize = 1u << 1;
constexpr uint32_t x11MotifFuncMove = 1u << 2;
constexpr uint32_t x11MotifFuncMinimize = 1u << 3;
constexpr uint32_t x11MotifFuncMaximize = 1u << 4;
constexpr uint32_t x11MotifFuncClose = 1u << 5;

template <typename T>
const T& x11As(const xcb_generic_event_t& event)
{
    return *reinterpret_cast<const T*>(&event);
}

// Not premultiplied and not transparency: this is the colour the server shows
// where nothing of ours has painted yet.
uint32_t x11BackgroundPixel(Color colour)
{
    auto channel = [](float value)
    { return (uint32_t) std::lround(std::clamp(value, 0.f, 1.f) * 255.f); };

    return (channel(colour.r) << 16) | (channel(colour.g) << 8) | channel(colour.b);
}

int x11WholePoints(float points)
{
    return std::max((int) std::lround(points), 1);
}

// A size the server sent and the size asked for instead of it, so the same
// disagreement is only ever argued once.
struct X11SizeRequest
{
    int fromWidth = 0;
    int fromHeight = 0;
    int width = 0;
    int height = 0;

    bool operator==(const X11SizeRequest&) const = default;
};

struct X11WindowNative final
    : LinuxWindowNative
    , X11WindowSurface
{
    X11WindowNative(const WindowOptions& options, WindowEvents& events)
        : state(*this, options, events)
        , borderless(options.flags.contains(WindowFlags::Borderless))
        , positionRequested(options.initialPosition.has_value())
    {
        state.unmap = [this] { unmap(); };

        onKeyboardFocus = [this](bool focused) { state.setActive(focused); };
        onConnectionLost = [this] { connectionLost(); };

        createWindow();
    }

    ~X11WindowNative() override
    {
        if (contentView != nullptr)
            linuxUnbindWindowFromContentView(*contentView);

        auto* connection = x11Connection();

        if (getWindow() == XCB_NONE || connection == nullptr)
            return;

        if (auto* seatInput = connection->getInput())
            seatInput->windowDestroyed(*this);

        connection->unregisterWindow(getWindow());

        if (connection->isConnected())
        {
            xcb_destroy_window(connection->getConnection(), getWindow());
            connection->flush();
        }

        setWindow(XCB_NONE);
    }

    void createWindow()
    {
        auto* connection = x11Connection();

        if (connection == nullptr || !connection->isConnected())
            return;

        auto* screen = connection->getScreen();

        if (screen == nullptr)
            return;

        auto* xcb = connection->getConnection();
        auto window = xcb_generate_id(xcb);

        const uint32_t values[] = {x11BackgroundPixel(state.background),
                                   x11ToplevelEventMask};

        xcb_create_window(xcb,
                          XCB_COPY_FROM_PARENT,
                          window,
                          screen->root,
                          x11ClampPosition(std::lround(state.position.x)),
                          x11ClampPosition(std::lround(state.position.y)),
                          x11ClampSize(contentWidth()),
                          x11ClampSize(contentHeight()),
                          0,
                          XCB_WINDOW_CLASS_INPUT_OUTPUT,
                          screen->root_visual,
                          XCB_CW_BACK_PIXEL | XCB_CW_EVENT_MASK,
                          values);

        setWindow(window);
        connection->registerWindow({window, this, nullptr});

        applyProtocols();
        applyTitle();
        applyIdentity();
        applySizeHints();
        applyMotifHints();

        connection->flush();
    }

    // The connection, and only while this window is a live id on it: every
    // request below would otherwise go to a window the server no longer has.
    X11Connection* liveConnection() const
    {
        auto* connection = x11Connection();

        if (connection == nullptr || !connection->isConnected()
            || getWindow() == XCB_NONE)
            return nullptr;

        return connection;
    }

    void applyProtocols()
    {
        auto* connection = liveConnection();

        if (connection == nullptr)
            return;

        const auto& atoms = connection->getAtoms();

        auto deleteWindow = atoms.wmDeleteWindow;

        xcb_icccm_set_wm_protocols(connection->getConnection(),
                                   getWindow(),
                                   atoms.wmProtocols,
                                   1,
                                   &deleteWindow);
    }

    void applyTitle()
    {
        auto* connection = liveConnection();

        if (connection == nullptr)
            return;

        auto* xcb = connection->getConnection();
        const auto& atoms = connection->getAtoms();
        const auto length = (uint32_t) state.title.size();

        // _NET_WM_NAME is what a modern window manager reads and WM_NAME what
        // everything older falls back to, so both carry the title.
        xcb_change_property(xcb,
                            XCB_PROP_MODE_REPLACE,
                            getWindow(),
                            atoms.netWmName,
                            atoms.utf8String,
                            8,
                            length,
                            state.title.c_str());

        xcb_change_property(xcb,
                            XCB_PROP_MODE_REPLACE,
                            getWindow(),
                            XCB_ATOM_WM_NAME,
                            XCB_ATOM_STRING,
                            8,
                            length,
                            state.title.c_str());
    }

    void applyIdentity()
    {
        auto* connection = liveConnection();

        if (connection == nullptr)
            return;

        auto* xcb = connection->getConnection();
        const auto& atoms = connection->getAtoms();

        // Instance and class, each NUL-terminated inside one property.
        auto wmClass = std::string {linuxDefaultAppId};
        wmClass.push_back('\0');
        wmClass += linuxDefaultAppId;
        wmClass.push_back('\0');

        xcb_icccm_set_wm_class(
            xcb, getWindow(), (uint32_t) wmClass.size(), wmClass.c_str());

        const uint32_t pid = (uint32_t) ::getpid();

        xcb_change_property(xcb,
                            XCB_PROP_MODE_REPLACE,
                            getWindow(),
                            atoms.netWmPid,
                            XCB_ATOM_CARDINAL,
                            32,
                            1,
                            &pid);

        xcb_change_property(xcb,
                            XCB_PROP_MODE_REPLACE,
                            getWindow(),
                            atoms.netWmWindowType,
                            XCB_ATOM_ATOM,
                            32,
                            1,
                            &atoms.netWmWindowTypeNormal);
    }

    void applySizeHints()
    {
        auto* connection = liveConnection();

        if (connection == nullptr)
            return;

        auto width = contentWidth();
        auto height = contentHeight();

        auto hints = xcb_size_hints_t {};

        if (positionRequested)
            xcb_icccm_size_hints_set_position(
                &hints,
                1,
                (int32_t) std::lround(state.position.x),
                (int32_t) std::lround(state.position.y));

        xcb_icccm_size_hints_set_size(&hints, 1, width, height);

        auto minWidth = std::max(state.minWidth, 1);
        auto minHeight = std::max(state.minHeight, 1);

        // Pinned rather than merely unset, the way the Wayland frame is: min
        // equal to max is the only way X11 says "this window does not resize".
        if (!state.resizable)
        {
            minWidth = width;
            minHeight = height;

            xcb_icccm_size_hints_set_max_size(&hints, width, height);
        }

        xcb_icccm_size_hints_set_min_size(&hints, minWidth, minHeight);

        if (state.aspectRatio)
        {
            const auto numerator =
                (int32_t) std::lround(state.aspectRatio->x * x11AspectPrecision);
            const auto denominator =
                (int32_t) std::lround(state.aspectRatio->y * x11AspectPrecision);

            xcb_icccm_size_hints_set_aspect(
                &hints, numerator, denominator, numerator, denominator);
        }

        xcb_icccm_set_wm_normal_hints(
            connection->getConnection(), getWindow(), &hints);
    }

    void applyMotifHints()
    {
        auto* connection = liveConnection();

        if (connection == nullptr)
            return;

        const auto& atoms = connection->getAtoms();

        if (atoms.motifWmHints == XCB_ATOM_NONE)
            return;

        auto hints = X11MotifHints {};

        if (borderless)
            hints.flags |= x11MotifHintsDecorations;

        if (!state.resizable || !state.closable || !state.miniaturizable)
        {
            hints.flags |= x11MotifHintsFunctions;
            hints.functions = x11MotifFuncMove;

            if (state.resizable)
                hints.functions |= x11MotifFuncResize | x11MotifFuncMaximize;

            if (state.miniaturizable)
                hints.functions |= x11MotifFuncMinimize;

            if (state.closable)
                hints.functions |= x11MotifFuncClose;
        }

        if (hints.flags == 0)
            return;

        xcb_change_property(connection->getConnection(),
                            XCB_PROP_MODE_REPLACE,
                            getWindow(),
                            atoms.motifWmHints,
                            atoms.motifWmHints,
                            32,
                            sizeof(X11MotifHints) / sizeof(uint32_t),
                            &hints);
    }

    void handleEvent(const xcb_generic_event_t& event) override
    {
        switch (event.response_type & ~0x80)
        {
            case XCB_MAP_NOTIFY:
                mapNotify(x11As<xcb_map_notify_event_t>(event));
                break;

            case XCB_UNMAP_NOTIFY:
                unmapNotify(x11As<xcb_unmap_notify_event_t>(event));
                break;

            case XCB_CONFIGURE_NOTIFY:
                configureNotify(x11As<xcb_configure_notify_event_t>(event),
                                (event.response_type & 0x80) != 0);
                break;

            case XCB_REPARENT_NOTIFY:
                reparentNotify(x11As<xcb_reparent_notify_event_t>(event));
                break;

            case XCB_DESTROY_NOTIFY:
                destroyNotify(x11As<xcb_destroy_notify_event_t>(event));
                break;

            case XCB_EXPOSE:
                expose(x11As<xcb_expose_event_t>(event));
                break;

            case XCB_FOCUS_IN:
                focusChanged(x11As<xcb_focus_in_event_t>(event), true);
                break;

            case XCB_FOCUS_OUT:
                focusChanged(x11As<xcb_focus_out_event_t>(event), false);
                break;

            case XCB_CLIENT_MESSAGE:
                clientMessage(x11As<xcb_client_message_event_t>(event));
                break;

            case XCB_PROPERTY_NOTIFY:
                propertyNotify(x11As<xcb_property_notify_event_t>(event));
                break;

            // Everything the seat says. The window has nothing to add: an
            // event that reached the toplevel is already in its points,
            // whether it was selected there or propagated up from a view's
            // child window.
            case XCB_KEY_PRESS:
            case XCB_KEY_RELEASE:
                if (auto* seatInput = getInput())
                    seatInput->keyChanged(*this,
                                          x11As<xcb_key_press_event_t>(event),
                                          (event.response_type & ~0x80)
                                              == XCB_KEY_PRESS);
                break;

            case XCB_BUTTON_PRESS:
            case XCB_BUTTON_RELEASE:
                if (auto* seatInput = getInput())
                    seatInput->buttonChanged(*this,
                                             x11As<xcb_button_press_event_t>(event),
                                             (event.response_type & ~0x80)
                                                 == XCB_BUTTON_PRESS);
                break;

            case XCB_MOTION_NOTIFY:
                if (auto* seatInput = getInput())
                    seatInput->pointerMoved(*this,
                                            x11As<xcb_motion_notify_event_t>(event));
                break;

            case XCB_ENTER_NOTIFY:
                if (auto* seatInput = getInput())
                    seatInput->pointerEntered(
                        *this, x11As<xcb_enter_notify_event_t>(event));
                break;

            case XCB_LEAVE_NOTIFY:
                if (auto* seatInput = getInput())
                    seatInput->pointerLeft(*this,
                                           x11As<xcb_leave_notify_event_t>(event));
                break;

            default:
                break;
        }
    }

    void mapNotify(const xcb_map_notify_event_t& event)
    {
        if (event.window != getWindow() || mapped)
            return;

        mapped = true;
        state.notifyHostVisibility(true);

        if (contentView != nullptr)
            linuxWindowSurfaceStateChanged(*contentView);
    }

    void unmapNotify(const xcb_unmap_notify_event_t& event)
    {
        if (event.window != getWindow() || !mapped)
            return;

        mapped = false;

        if (contentView != nullptr)
            linuxWindowSurfaceStateChanged(*contentView);

        state.setActive(false);
        state.notifyHostVisibility(false);
    }

    void configureNotify(const xcb_configure_notify_event_t& event, bool synthetic)
    {
        if (event.window != getWindow())
            return;

        auto width = (int) event.width;
        auto height = (int) event.height;

        state.applyConstraints(width, height);

        // Nothing negotiates a size on X11: the server has resized already, so
        // a constraint that disagrees is asked for back rather than agreed in
        // advance. Once per size the server sent, though - a window manager
        // that enforces a geometry of its own answers the request with the
        // size it sent in the first place, and a request per answer would
        // never end.
        if (width != (int) event.width || height != (int) event.height)
            askForSize({(int) event.width, (int) event.height, width, height});
        else
            lastSizeRequest.reset();

        state.resizeTo({(float) width, (float) height});

        updatePosition(event, synthetic);

        if (contentView != nullptr)
            linuxWindowSurfaceStateChanged(*contentView);
    }

    void askForSize(const X11SizeRequest& request)
    {
        if (lastSizeRequest == request)
            return;

        lastSizeRequest = request;

        resizeWindowTo(request.width, request.height);
    }

    // The event's x and y are the parent's coordinates, which are the root's
    // until a reparenting window manager puts a frame around us; a window
    // manager's own synthetic configure is root-relative by definition
    // (ICCCM 4.2.3). Only the reparented real event is worth a round trip -
    // one per frame of a drag otherwise.
    void updatePosition(const xcb_configure_notify_event_t& event, bool synthetic)
    {
        if (synthetic || parentIsRoot)
        {
            reportPosition({(float) event.x, (float) event.y});
            return;
        }

        updatePositionFromServer();
    }

    void updatePositionFromServer()
    {
        auto* connection = liveConnection();

        if (connection == nullptr || connection->getScreen() == nullptr)
            return;

        auto* xcb = connection->getConnection();

        auto* reply = xcb_translate_coordinates_reply(
            xcb,
            xcb_translate_coordinates(
                xcb, getWindow(), connection->getScreen()->root, 0, 0),
            nullptr);

        if (reply == nullptr)
            return;

        const auto position = Point {(float) reply->dst_x, (float) reply->dst_y};
        std::free(reply);

        reportPosition(position);
    }

    void reportPosition(Point position)
    {
        if (position.x == state.position.x && position.y == state.position.y)
            return;

        state.setPosition(position);
    }

    // A reparenting window manager frames the toplevel on the way up, and from
    // then on a real ConfigureNotify names the frame's coordinates rather than
    // the root's. Rare enough to be worth the round trip once.
    void reparentNotify(const xcb_reparent_notify_event_t& event)
    {
        if (event.window != getWindow())
            return;

        auto* connection = x11Connection();
        auto* screen = connection != nullptr ? connection->getScreen() : nullptr;

        parentIsRoot = screen == nullptr || event.parent == screen->root;

        updatePositionFromServer();
    }

    // Somebody else destroyed the toplevel - a KillClient on it, or a window
    // manager taking a frame down with our window inside. The id is dead, so
    // nothing more may be sent to it, and what is left is the window a
    // headless build has.
    void destroyNotify(const xcb_destroy_notify_event_t& event)
    {
        if (event.window != getWindow())
            return;

        if (auto* connection = x11Connection())
            connection->unregisterWindow(getWindow());

        markWindowGone();
    }

    void expose(const xcb_expose_event_t& event)
    {
        // An exposed region arrives in pieces; only the last one is worth a
        // repaint of the whole surface.
        if (event.count != 0)
            return;

        auto* connection = x11Connection();

        if (connection == nullptr)
            return;

        // A presenting view's own child window. The toplevel's background is
        // the server's to paint from its back_pixel, so an Expose on it asks
        // nothing of us.
        if (auto* view = connection->findWindow(event.window).view)
            view->repaint();
    }

    // The seat decides what a focus event means, because the pressed keys and
    // the mouse lock it holds move with it; with no seat there is still an
    // active state to report.
    void focusChanged(const xcb_focus_in_event_t& event, bool focused)
    {
        if (event.event != getWindow())
            return;

        if (auto* seatInput = getInput())
            seatInput->focusChanged(*this, event, focused);
        else
            onKeyboardFocus(focused);
    }

    void clientMessage(const xcb_client_message_event_t& event)
    {
        auto* connection = x11Connection();

        if (connection == nullptr || event.window != getWindow())
            return;

        const auto& atoms = connection->getAtoms();

        if (event.type != atoms.wmProtocols || event.format != 32)
            return;

        if (event.data.data32[0] == atoms.wmDeleteWindow)
            state.closeRequested();
    }

    void propertyNotify(const xcb_property_notify_event_t& event)
    {
        auto* connection = x11Connection();

        if (connection == nullptr || event.window != getWindow())
            return;

        if (event.atom != connection->getAtoms().netWmState)
            return;

        state.maximized = readMaximized();
    }

    // Whatever the property could not answer leaves the state as it was: an
    // absent or malformed _NET_WM_STATE says nothing about the window, and
    // reading it as "not maximized" would be an unmaximize nobody asked for.
    bool readMaximized()
    {
        auto* connection = liveConnection();

        if (connection == nullptr)
            return state.maximized;

        auto* xcb = connection->getConnection();
        const auto& atoms = connection->getAtoms();

        auto* reply = xcb_get_property_reply(
            xcb,
            xcb_get_property(
                xcb, 0, getWindow(), atoms.netWmState, XCB_ATOM_ATOM, 0, 64),
            nullptr);

        if (reply == nullptr)
            return state.maximized;

        auto maximized = state.maximized;

        if (reply->type == XCB_ATOM_ATOM && reply->format == 32)
        {
            const auto* values =
                static_cast<const xcb_atom_t*>(xcb_get_property_value(reply));
            const auto count = xcb_get_property_value_length(reply) / 4;

            auto horizontal = false;
            auto vertical = false;

            for (auto i = 0; i < count; ++i)
            {
                horizontal =
                    horizontal || values[i] == atoms.netWmStateMaximizedHorz;
                vertical = vertical || values[i] == atoms.netWmStateMaximizedVert;
            }

            maximized = horizontal && vertical;
        }

        std::free(reply);

        return maximized;
    }

    void setVisible(bool shouldBeVisible) override
    {
        if (getWindow() == XCB_NONE)
            return;

        if (shouldBeVisible)
            map();
        else
            unmap();
    }

    // mapped waits for the MapNotify: until the server says so there is
    // nothing on screen for a view surface to sit on.
    void map()
    {
        auto* connection = x11Connection();

        if (connection == nullptr || !connection->isConnected())
            return;

        xcb_map_window(connection->getConnection(), getWindow());
        connection->flush();
    }

    // Eagerly, unlike the map: the view surfaces have to go before the request
    // reaches the server, and the UnmapNotify that follows finds it done.
    void unmap()
    {
        auto wasMapped = mapped;
        mapped = false;

        if (contentView != nullptr)
            linuxWindowSurfaceStateChanged(*contentView);

        state.setActive(false);

        auto* connection = x11Connection();

        if (connection != nullptr && connection->isConnected()
            && getWindow() != XCB_NONE)
        {
            xcb_unmap_window(connection->getConnection(), getWindow());
            connection->flush();
        }

        if (wasMapped)
            state.notifyHostVisibility(false);
    }

    void setTitle(const std::string& newTitle) override
    {
        state.title = newTitle;

        applyTitle();

        if (auto* connection = x11Connection())
            connection->flush();
    }

    // The xcb_window_t widened into a pointer: an X11 id is a number, and
    // every API that hands one out passes it as a void*. Null before the
    // window exists and after the connection has gone.
    void* getHandle() override
    {
        return reinterpret_cast<void*>((uintptr_t) getWindow());
    }

    void minimize() override
    {
        auto* connection = x11Connection();

        if (connection == nullptr)
            return;

        sendRootClientMessage(connection->getAtoms().wmChangeState,
                              {XCB_ICCCM_WM_STATE_ICONIC, 0, 0, 0, 0});
    }

    void toggleMaximize() override
    {
        auto* connection = x11Connection();

        if (connection == nullptr)
            return;

        const auto& atoms = connection->getAtoms();

        sendRootClientMessage(atoms.netWmState,
                              {x11NetWmStateToggle,
                               atoms.netWmStateMaximizedHorz,
                               atoms.netWmStateMaximizedVert,
                               x11ClientSourceApplication,
                               0});
    }

    // Everything a window manager acts on is a message to the root, where its
    // SubstructureRedirect selection picks it up; with no window manager there
    // is nobody listening and nothing happens.
    void sendRootClientMessage(xcb_atom_t type, const std::array<uint32_t, 5>& data)
    {
        auto* connection = x11Connection();

        if (connection == nullptr || !connection->isConnected()
            || getWindow() == XCB_NONE || type == XCB_ATOM_NONE
            || connection->getScreen() == nullptr)
            return;

        auto event = xcb_client_message_event_t {};
        event.response_type = XCB_CLIENT_MESSAGE;
        event.format = 32;
        event.window = getWindow();
        event.type = type;

        for (auto i = size_t {0}; i < data.size(); ++i)
            event.data.data32[i] = data[i];

        xcb_send_event(connection->getConnection(),
                       0,
                       connection->getScreen()->root,
                       XCB_EVENT_MASK_SUBSTRUCTURE_NOTIFY
                           | XCB_EVENT_MASK_SUBSTRUCTURE_REDIRECT,
                       reinterpret_cast<const char*>(&event));

        connection->flush();
    }

    // Reported at once, the way every other backend reports it, so the value
    // put in is the one that comes back out; a window manager that puts the
    // window somewhere else says so in the ConfigureNotify that answers, and
    // that correction is what is reported then.
    void setPosition(Point newPosition) override
    {
        LinuxWindowNative::setPosition(newPosition);

        auto* connection = liveConnection();

        if (connection == nullptr)
            return;

        const uint32_t values[] = {
            (uint32_t) (int32_t) x11ClampPosition(std::lround(newPosition.x)),
            (uint32_t) (int32_t) x11ClampPosition(std::lround(newPosition.y))};

        xcb_configure_window(connection->getConnection(),
                             getWindow(),
                             XCB_CONFIG_WINDOW_X | XCB_CONFIG_WINDOW_Y,
                             values);
        connection->flush();
    }

    void resizeWindowTo(int width, int height)
    {
        auto* connection = liveConnection();

        if (connection == nullptr)
            return;

        const uint32_t values[] = {x11ClampSize(width), x11ClampSize(height)};

        xcb_configure_window(connection->getConnection(),
                             getWindow(),
                             XCB_CONFIG_WINDOW_WIDTH | XCB_CONFIG_WINDOW_HEIGHT,
                             values);
        connection->flush();
    }

    void setMouseLocked(bool locked) override
    {
        mouseLockIntent = locked;

        if (auto* seatInput = getInput())
            seatInput->updateMouseLock(*this);
    }

    bool isKeyPressed(uint16_t nativeKeyCode) override
    {
        if (auto* seatInput = focusedInput())
            return seatInput->isKeyPressed(nativeKeyCode);

        return false;
    }

    ModifierKeys getModifiers() override
    {
        if (auto* seatInput = focusedInput())
            return seatInput->getModifiers();

        return {};
    }

    static X11Input* getInput()
    {
        auto* connection = x11Connection();

        return connection != nullptr ? connection->getInput() : nullptr;
    }

    // The seat, and only while the keys it is reporting are going to this
    // window.
    X11Input* focusedInput() const
    {
        auto* seatInput = getInput();

        if (seatInput == nullptr || seatInput->getKeyboardFocus() != this)
            return nullptr;

        return seatInput;
    }

    // The server went away. Everything made from the connection goes with it,
    // the view surfaces included; the connection clears its own map, so only
    // this window's own record of itself is left to drop.
    void connectionLost() { markWindowGone(); }

    // The window id is not ours any more, whichever way it went. The view
    // surfaces' onLost has to fire while their window ids are still the ones
    // the records hold, so the state change goes first; what is left is the
    // window a headless build has.
    void markWindowGone()
    {
        auto wasMapped = mapped;
        mapped = false;

        if (contentView != nullptr)
            linuxWindowSurfaceStateChanged(*contentView);

        state.setActive(false);

        // The destructor's own call is behind a window id this is about to
        // clear, so the seat is told here instead.
        if (auto* seatInput = getInput())
            seatInput->windowDestroyed(*this);

        setWindow(XCB_NONE);

        if (wasMapped)
            state.notifyHostVisibility(false);
    }

    int contentWidth() const { return x11WholePoints(contentSize.x); }
    int contentHeight() const { return x11WholePoints(contentSize.y); }

    LinuxWindowState& getState() override { return state; }

    LinuxWindowState state;

    // False from the first ReparentNotify a window manager sends, after which
    // a real ConfigureNotify carries the frame's coordinates and not the
    // root's.
    bool parentIsRoot = true;

    std::optional<X11SizeRequest> lastSizeRequest;

    bool borderless = false;
    bool positionRequested = false;
};
} // namespace

std::unique_ptr<LinuxWindowNative> makeX11WindowNative(const WindowOptions& options,
                                                       WindowEvents& events)
{
    return std::make_unique<X11WindowNative>(options, events);
}
} // namespace eacp::Graphics
