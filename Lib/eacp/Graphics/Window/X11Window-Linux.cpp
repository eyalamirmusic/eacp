#include "X11Window-Linux.h"

#include "X11Connection-Linux.h"
#include "X11Input-Linux.h"

#include <eacp/Core/Threads/EventLoop.h>
#include <eacp/Core/Utils/Logging.h>

#include <xcb/xcb_icccm.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <memory>
#include <optional>
#include <string>
#include <unistd.h>

// File-scope names carry an x11/X11 prefix: this is one unity TU under
// EACP_CI_BUILD.

namespace eacp::Graphics
{
namespace
{
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

// The server measures a toplevel in pixels and the framework in points, so
// every size and every position crossing that line is converted here and
// rounded once, at the crossing. A window of at least one pixel, because the
// protocol has no zero-sized one.
int x11PixelSize(float points, float scale)
{
    return std::max((int) std::lround(points * scale), 1);
}

int x11PixelOrigin(float points, float scale)
{
    return (int) std::lround((double) points * (double) scale);
}

int x11PointSize(int pixels, float scale)
{
    return std::max((int) std::lround((double) pixels / (double) scale), 1);
}

int x11WholePoints(float points)
{
    return std::max((int) std::lround(points), 1);
}

// What a popup's grab has to keep reporting: a press over a window that is not
// ours arrives here and nowhere else, which is how a click outside a menu
// closes it without also pressing whatever it landed on.
constexpr uint16_t x11PopupGrabMask =
    XCB_EVENT_MASK_BUTTON_PRESS | XCB_EVENT_MASK_BUTTON_RELEASE
    | XCB_EVENT_MASK_POINTER_MOTION | XCB_EVENT_MASK_ENTER_WINDOW
    | XCB_EVENT_MASK_LEAVE_WINDOW;

// Another client's menu may be holding the pointer: the loop is run for this
// long between tries so the press that takes it down is heard.
constexpr auto x11PopupGrabRetryWait = Time::MS {20};
constexpr auto x11PopupGrabAttempts = 5;

// A window tree is a handful of levels deep at most; the bound is there so a
// cycle a broken host reports cannot hang the walk.
constexpr auto x11MaxTreeDepth = 32;

// The id the options name, whichever way they name it: an eacp Window widens
// its own into a pointer, and a host hands one over already widened.
xcb_window_t x11RequestedOwner(const WindowOptions& options)
{
    if (options.parent != nullptr)
        return (xcb_window_t) (uintptr_t) options.parent->getHandle();

    return (xcb_window_t) (uintptr_t) options.nativeParent;
}

// The toplevel an id belongs to: a host may hand over a child window inside
// its editor, and WM_TRANSIENT_FOR names the window a window manager knows.
xcb_window_t x11ToplevelOf(X11Connection& connection, xcb_window_t window)
{
    auto* screen = connection.getScreen();

    if (screen == nullptr || window == XCB_NONE || window == screen->root)
        return window;

    auto* xcb = connection.getConnection();
    auto current = window;

    for (auto step = 0; step < x11MaxTreeDepth; ++step)
    {
        auto* reply =
            xcb_query_tree_reply(xcb, xcb_query_tree(xcb, current), nullptr);

        if (reply == nullptr)
            return current;

        const auto parent = reply->parent;
        std::free(reply);

        if (parent == XCB_NONE || parent == screen->root)
            return current;

        current = parent;
    }

    return current;
}

// The state a queued dismissal holds, weakly: its owner is the popup's native,
// so one already on the loop when the app destroyed the window finds nothing
// and does nothing.
struct X11PopupDismissal
{
    WindowEvents* events = nullptr;
    bool pending = false;
};

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
    , X11PopupGrab
{
    X11WindowNative(const WindowOptions& options, WindowEvents& events)
        : state(*this, options, events)
        , borderless(options.flags.contains(WindowFlags::Borderless))
        , positionRequested(options.initialPosition.has_value())
        , popup(options.popup)
        , alwaysOnTop(options.alwaysOnTop)
        , dismissOnOutsideClick(options.dismissOnOutsideClick)
        , requestedOwner(x11RequestedOwner(options))
    {
        state.unmap = [this] { unmap(); };

        refusesFocus = popup;

        onKeyboardFocus = [this](bool focused) { state.setActive(focused); };
        onConnectionLost = [this] { connectionLost(); };

        if (popup)
        {
            dismissal = std::make_shared<X11PopupDismissal>();
            dismissal->events = &events;
        }

        if (auto* connection = x11Connection())
            scale = connection->getScale();

        createWindow();
    }

    ~X11WindowNative() override
    {
        // Before anything else: a dismissal already queued must find the state
        // gone rather than a window half way through being torn down.
        releasePopupGrab();
        dismissal.reset();

        if (contentView != nullptr)
            linuxUnbindWindowFromContentView(*contentView);

        auto* connection = x11Connection();

        if (connection == nullptr)
            return;

        connection->unwatchForeignWindow(*this);

        if (getWindow() == XCB_NONE)
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

        ownerWindow = x11ToplevelOf(*connection, requestedOwner);

        // In the order the mask names them, which is the order the protocol
        // reads them in.
        auto values = std::array<uint32_t, 4> {};
        auto count = size_t {0};
        auto mask = uint32_t {XCB_CW_BACK_PIXEL};

        values[count++] = x11BackgroundPixel(state.background);

        // A popup is placed and stacked by us and not by a window manager, and
        // the server keeps what it covers where it can, so a menu taken down
        // leaves no hole for the window under it to repaint.
        if (popup)
        {
            mask |= XCB_CW_OVERRIDE_REDIRECT | XCB_CW_SAVE_UNDER;
            values[count++] = 1;
            values[count++] = 1;
        }

        mask |= XCB_CW_EVENT_MASK;
        values[count++] = connection->getWindowEventMask();

        xcb_create_window(xcb,
                          XCB_COPY_FROM_PARENT,
                          window,
                          screen->root,
                          x11ClampPosition(pixelX()),
                          x11ClampPosition(pixelY()),
                          x11ClampSize(pixelWidth()),
                          x11ClampSize(pixelHeight()),
                          0,
                          XCB_WINDOW_CLASS_INPUT_OUTPUT,
                          screen->root_visual,
                          mask,
                          values.data());

        setWindow(window);
        connection->registerWindow({window, this, nullptr});
        connection->selectPointerEvents(window);

        applyProtocols();
        applyTitle();
        applyIdentity();

        // None of which a popup has: an override-redirect window is never
        // decorated, never resized by anybody else and never negotiated over,
        // so the hints a window manager would read are not written at all.
        if (!popup)
        {
            applySizeHints();
            applyMotifHints();
        }

        watchOwner();

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

        const auto type = popup && atoms.netWmWindowTypePopupMenu != XCB_ATOM_NONE
                              ? atoms.netWmWindowTypePopupMenu
                              : atoms.netWmWindowTypeNormal;

        xcb_change_property(xcb,
                            XCB_PROP_MODE_REPLACE,
                            getWindow(),
                            atoms.netWmWindowType,
                            XCB_ATOM_ATOM,
                            32,
                            1,
                            &type);

        applyTransientFor();
        applyAlwaysOnTop();
    }

    // Written for any window given an owner, popup or not: it is what tells a
    // window manager which window this one belongs to, so it is kept above it
    // and taken down with it.
    void applyTransientFor()
    {
        auto* connection = liveConnection();

        if (connection == nullptr || ownerWindow == XCB_NONE)
            return;

        xcb_icccm_set_wm_transient_for(
            connection->getConnection(), getWindow(), ownerWindow);
    }

    // _NET_WM_STATE is the window manager's to change once a window is up, and
    // the client's to state before it maps - which is the only moment this
    // window has to say it.
    void applyAlwaysOnTop()
    {
        auto* connection = liveConnection();

        if (connection == nullptr || !alwaysOnTop)
            return;

        const auto& atoms = connection->getAtoms();

        xcb_change_property(connection->getConnection(),
                            XCB_PROP_MODE_REPLACE,
                            getWindow(),
                            atoms.netWmState,
                            XCB_ATOM_ATOM,
                            32,
                            1,
                            &atoms.netWmStateAbove);
    }

    // The owner is watched and never touched. What a popup has to hear about
    // is the window it pops over going away, being hidden, or no longer being
    // the one the keyboard goes to: each of those is the end of the menu.
    void watchOwner()
    {
        auto* connection = liveConnection();

        if (!popup || connection == nullptr || ownerWindow == XCB_NONE)
            return;

        // Unless the owner is a window of this copy's, whose own mask this
        // would replace: it already selects both of these, and the watch alone
        // is what brings its events here as well.
        if (connection->findWindow(ownerWindow).windowSurface == nullptr)
        {
            const uint32_t mask =
                XCB_EVENT_MASK_STRUCTURE_NOTIFY | XCB_EVENT_MASK_FOCUS_CHANGE;

            xcb_change_window_attributes(
                connection->getConnection(), ownerWindow, XCB_CW_EVENT_MASK, &mask);
        }

        connection->watchForeignWindow({ownerWindow, this, nullptr});
    }

    void applySizeHints()
    {
        auto* connection = liveConnection();

        if (connection == nullptr)
            return;

        // Every number in the hints is a pixel to a window manager, this
        // window's own minimum included.
        auto width = pixelWidth();
        auto height = pixelHeight();

        auto hints = xcb_size_hints_t {};

        if (positionRequested)
            xcb_icccm_size_hints_set_position(
                &hints, 1, (int32_t) pixelX(), (int32_t) pixelY());

        xcb_icccm_size_hints_set_size(&hints, 1, width, height);

        auto minWidth = x11PixelSize((float) std::max(state.minWidth, 1), scale);
        auto minHeight = x11PixelSize((float) std::max(state.minHeight, 1), scale);

        // Pinned rather than merely unset, the way the Wayland frame is: min
        // equal to max is the only way X11 says "this window does not resize".
        if (!state.resizable)
        {
            minWidth = width;
            minHeight = height;

            xcb_icccm_size_hints_set_max_size(&hints, width, height);
        }

        xcb_icccm_size_hints_set_min_size(&hints, minWidth, minHeight);

        if (state.aspectRatioHint)
        {
            const auto numerator =
                (int32_t) std::lround(state.aspectRatioHint->x * x11AspectPrecision);
            const auto denominator =
                (int32_t) std::lround(state.aspectRatioHint->y * x11AspectPrecision);

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
        if (handleOwnerEvent(event))
            return;

        if (handleGrabBroken(event))
            return;

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

    // The owner's own events, routed here by the watch beside whatever the
    // owner itself made of them. A grab or the pointer crossing is neither the
    // owner losing the user's attention nor the popup's business, exactly as
    // the seat reads a focus event.
    bool handleOwnerEvent(const xcb_generic_event_t& event)
    {
        if (!popup || ownerWindow == XCB_NONE)
            return false;

        switch (event.response_type & ~0x80)
        {
            case XCB_UNMAP_NOTIFY:
                if (x11As<xcb_unmap_notify_event_t>(event).window != ownerWindow)
                    return false;

                break;

            case XCB_DESTROY_NOTIFY:
                if (x11As<xcb_destroy_notify_event_t>(event).window != ownerWindow)
                    return false;

                break;

            case XCB_FOCUS_OUT:
            {
                const auto& focus = x11As<xcb_focus_out_event_t>(event);

                if (focus.event != ownerWindow || focus.mode == XCB_NOTIFY_MODE_GRAB
                    || focus.mode == XCB_NOTIFY_MODE_UNGRAB
                    || focus.detail == XCB_NOTIFY_DETAIL_POINTER)
                    return false;

                break;
            }

            default:
                return false;
        }

        requestDismissal();

        return true;
    }

    // The grab ended without this window asking: the server takes one back
    // when the window it is on stops being viewable, and a menu with no
    // pointer is a menu that can no longer be clicked away.
    bool handleGrabBroken(const xcb_generic_event_t& event)
    {
        if (!pointerGrabbed || (event.response_type & ~0x80) != XCB_LEAVE_NOTIFY)
            return false;

        const auto& crossing = x11As<xcb_leave_notify_event_t>(event);

        if (crossing.event != getWindow() || crossing.mode != XCB_NOTIFY_MODE_UNGRAB)
            return false;

        requestDismissal();

        return true;
    }

    // Deferred a turn, and the callable copied before it runs: the handler's
    // whole job is to destroy or hide the window it belongs to, and it is a
    // member of that window.
    void requestDismissal() override
    {
        if (dismissal == nullptr || dismissal->pending)
            return;

        dismissal->pending = true;

        auto deliver = [weak = std::weak_ptr<X11PopupDismissal> {dismissal}]
        {
            auto state = weak.lock();

            if (state == nullptr)
                return;

            state->pending = false;

            auto handler = state->events->onDismissRequested;
            handler();
        };

        Threads::callAsync(deliver);
    }

    bool dismissesOnOutsideClick() const override { return dismissOnOutsideClick; }

    // An override-redirect window is where it asked to be, so its own numbers
    // are the server's: no round trip per press.
    bool containsRootPoint(Point rootPosition) const override
    {
        const auto x = (float) pixelX();
        const auto y = (float) pixelY();

        return rootPosition.x >= x && rootPosition.y >= y
               && rootPosition.x < x + (float) pixelWidth()
               && rootPosition.y < y + (float) pixelHeight();
    }

    // The pointer for as long as the menu is up. owner_events is on, unlike
    // the mouse lock's: a press over a window of ours is still delivered
    // there, and the seat measures those against this window's rectangle
    // instead - so a press inside the popup reaches its views unchanged.
    void takePopupGrab()
    {
        auto* connection = liveConnection();

        if (!popup || popupActive || connection == nullptr || !mapped)
            return;

        popupActive = true;
        connection->setActivePopup(*this);

        if (dismissOnOutsideClick)
            pointerGrabbed = grabPointerForPopup(*connection);

        keyboardGrabbed = grabKeyboardForPopup(*connection);

        // Whatever the owner's views were holding, they are not holding it any
        // more: the up that would have ended it belongs to the menu.
        if (auto* seatInput = getInput())
            seatInput->popupGrabChanged();
    }

    void releasePopupGrab()
    {
        if (!popupActive)
            return;

        popupActive = false;

        auto* connection = x11Connection();

        if (connection == nullptr)
            return;

        connection->clearActivePopup(*this);

        if (connection->isConnected())
        {
            auto* xcb = connection->getConnection();

            if (pointerGrabbed)
                xcb_ungrab_pointer(xcb, XCB_CURRENT_TIME);

            if (keyboardGrabbed)
                xcb_ungrab_keyboard(xcb, XCB_CURRENT_TIME);

            connection->flush();
        }

        pointerGrabbed = false;
        keyboardGrabbed = false;

        if (auto* seatInput = connection->getInput())
            seatInput->popupGrabChanged();
    }

    bool grabPointerForPopup(X11Connection& connection)
    {
        for (auto attempt = 0; attempt < x11PopupGrabAttempts; ++attempt)
        {
            auto* xcb = connection.getConnection();

            auto* reply =
                xcb_grab_pointer_reply(xcb,
                                       xcb_grab_pointer(xcb,
                                                        1,
                                                        getWindow(),
                                                        x11PopupGrabMask,
                                                        XCB_GRAB_MODE_ASYNC,
                                                        XCB_GRAB_MODE_ASYNC,
                                                        XCB_NONE,
                                                        XCB_CURSOR_NONE,
                                                        XCB_CURRENT_TIME),
                                       nullptr);

            if (reply == nullptr)
                break;

            const auto status = reply->status;
            std::free(reply);

            if (status == XCB_GRAB_STATUS_SUCCESS)
                return true;

            // Frozen or not viewable is a window that will not carry a grab at
            // all; only another client's menu is worth waiting out.
            if (status != XCB_GRAB_STATUS_ALREADY_GRABBED)
                break;

            auto never = [] { return false; };
            connection.dispatchUntil(never, Time::Deadline {x11PopupGrabRetryWait});
        }

        LOG("X11: the pointer could not be grabbed for a popup window, so a "
            "press over another client's window will not dismiss it. Escape "
            "and a press over one of this app's own windows still do.");

        return false;
    }

    // So Escape reaches the menu wherever the keyboard is pointing. Taken with
    // owner_events on, which changes nothing about focus: the window the popup
    // pops over stays the focused one and still receives its own keys, and the
    // seat picks Escape out of whichever window it lands in.
    bool grabKeyboardForPopup(X11Connection& connection)
    {
        auto* xcb = connection.getConnection();

        auto* reply = xcb_grab_keyboard_reply(xcb,
                                              xcb_grab_keyboard(xcb,
                                                                1,
                                                                getWindow(),
                                                                XCB_CURRENT_TIME,
                                                                XCB_GRAB_MODE_ASYNC,
                                                                XCB_GRAB_MODE_ASYNC),
                                              nullptr);

        if (reply == nullptr)
            return false;

        const auto granted = reply->status == XCB_GRAB_STATUS_SUCCESS;
        std::free(reply);

        return granted;
    }

    // Xft.dpi changed under an open window. The content keeps the point size
    // it was laid out at, exactly as a Wayland toplevel keeps its logical
    // size, so what moves is the pixels: the window is asked for the size that
    // many points now needs, and every view surface is rebuilt against the new
    // scale. A window manager that refuses the resize answers with the size it
    // is keeping, and the ConfigureNotify turns that back into points.
    void scaleChanged(float newScale) override
    {
        if (newScale <= 0.f || newScale == scale)
            return;

        scale = newScale;

        applySizeHints();

        if (!state.maximized)
        {
            lastSizeRequest.reset();
            resizeWindowTo(x11WholePoints(contentSize.x),
                           x11WholePoints(contentSize.y));
        }

        if (contentView == nullptr)
            return;

        // The surfaces take the new scale first, so the notification after them
        // reads it rather than the one it replaces.
        linuxWindowSurfaceStateChanged(*contentView);
        notifyBackingScaleChanged(*contentView);
    }

    void mapNotify(const xcb_map_notify_event_t& event)
    {
        if (event.window != getWindow() || mapped)
            return;

        mapped = true;

        // Once the window is really on screen: a grab on one the server has
        // not mapped yet is refused as not viewable.
        takePopupGrab();

        state.notifyHostVisibility(true);

        if (contentView != nullptr)
            linuxWindowSurfaceStateChanged(*contentView);
    }

    void unmapNotify(const xcb_unmap_notify_event_t& event)
    {
        if (event.window != getWindow() || !mapped)
            return;

        mapped = false;
        releasePopupGrab();

        if (contentView != nullptr)
            linuxWindowSurfaceStateChanged(*contentView);

        state.setActive(false);
        state.notifyHostVisibility(false);
    }

    void configureNotify(const xcb_configure_notify_event_t& event, bool synthetic)
    {
        if (event.window != getWindow())
            return;

        // The wire is pixels and everything below here is points, the
        // constraint and the size request included.
        const auto givenWidth = x11PointSize((int) event.width, scale);
        const auto givenHeight = x11PointSize((int) event.height, scale);

        auto width = givenWidth;
        auto height = givenHeight;

        // A maximised toplevel is being given a ceiling, not dragged.
        state.applyConstraints(width, height, state.maximized);

        // Nothing negotiates a size on X11: the server has resized already, so
        // a constraint that disagrees is asked for back rather than agreed in
        // advance. Once per size the server sent, though - a window manager
        // that enforces a geometry of its own answers the request with the
        // size it sent in the first place, and a request per answer would
        // never end.
        if (width != givenWidth || height != givenHeight)
            askForSize({givenWidth, givenHeight, width, height});
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
            reportPosition(pointsFromPixels(event.x, event.y));
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

        const auto position = pointsFromPixels(reply->dst_x, reply->dst_y);
        std::free(reply);

        reportPosition(position);
    }

    // A screen position the server gave, in the points WindowOptions and
    // Display are in.
    Point pointsFromPixels(int x, int y) const
    {
        const auto divisor = scale > 0.f ? scale : 1.f;

        return {(float) x / divisor, (float) y / divisor};
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

        // Before the view surfaces are told: their own windows were inside
        // this one and the server took them with it.
        inferiorsGone = true;

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

        // The place is asked for again right before the map, and honoured
        // verbatim because nothing redirects an override-redirect window: a
        // menu opens where the click was, which may have been decided after
        // the window was made.
        if (popup)
            applyPopupPosition();

        xcb_map_window(connection->getConnection(), getWindow());
        connection->flush();
    }

    void applyPopupPosition()
    {
        auto* connection = liveConnection();

        if (connection == nullptr)
            return;

        const uint32_t values[] = {(uint32_t) (int32_t) x11ClampPosition(pixelX()),
                                   (uint32_t) (int32_t) x11ClampPosition(pixelY())};

        xcb_configure_window(connection->getConnection(),
                             getWindow(),
                             XCB_CONFIG_WINDOW_X | XCB_CONFIG_WINDOW_Y,
                             values);
    }

    // Eagerly, unlike the map: the view surfaces have to go before the request
    // reaches the server, and the UnmapNotify that follows finds it done.
    void unmap()
    {
        auto wasMapped = mapped;
        mapped = false;

        releasePopupGrab();

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

    // A maximised toplevel holds the size the window manager gave it, so
    // there is nothing to ask for. Otherwise the content size is taken first
    // - the hints are derived from it, and a non-resizable window is pinned
    // to the old size until they are rewritten - and the server is then asked
    // to catch up. The ConfigureNotify that answers carries this same size,
    // where resizeTo is silent, or the size a window manager insisted on
    // instead, which is a real resize and reports itself.
    void setSize(Point newSize) override
    {
        if (state.maximized)
            return;

        auto width = std::max((int) std::lround(newSize.x), 1);
        auto height = std::max((int) std::lround(newSize.y), 1);

        state.resizeTo({(float) width, (float) height});

        // This size is ours, not an answer to one the server sent, so it
        // leaves no request for configureNotify to match against.
        lastSizeRequest.reset();

        applySizeHints();
        resizeWindowTo(width, height);
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

        const uint32_t values[] = {(uint32_t) (int32_t) x11ClampPosition(
                                       x11PixelOrigin(newPosition.x, scale)),
                                   (uint32_t) (int32_t) x11ClampPosition(
                                       x11PixelOrigin(newPosition.y, scale))};

        xcb_configure_window(connection->getConnection(),
                             getWindow(),
                             XCB_CONFIG_WINDOW_X | XCB_CONFIG_WINDOW_Y,
                             values);
        connection->flush();
    }

    // In points, like everything that calls it.
    void resizeWindowTo(int width, int height)
    {
        auto* connection = liveConnection();

        if (connection == nullptr)
            return;

        const uint32_t values[] = {
            x11ClampSize(x11PixelSize((float) width, scale)),
            x11ClampSize(x11PixelSize((float) height, scale))};

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

        releasePopupGrab();

        if (auto* connection = x11Connection())
            connection->unwatchForeignWindow(*this);

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

    int pixelWidth() const { return x11PixelSize(contentSize.x, scale); }
    int pixelHeight() const { return x11PixelSize(contentSize.y, scale); }
    int pixelX() const { return x11PixelOrigin(state.position.x, scale); }
    int pixelY() const { return x11PixelOrigin(state.position.y, scale); }

    LinuxWindowState& getState() override { return state; }

    LinuxWindowState state;

    // False from the first ReparentNotify a window manager sends, after which
    // a real ConfigureNotify carries the frame's coordinates and not the
    // root's.
    bool parentIsRoot = true;

    std::optional<X11SizeRequest> lastSizeRequest;

    bool borderless = false;
    bool positionRequested = false;

    bool popup = false;
    bool alwaysOnTop = false;
    bool dismissOnOutsideClick = true;

    // The id the options named, and the toplevel it turned out to belong to -
    // a host may hand over a child window inside its editor.
    xcb_window_t requestedOwner = XCB_NONE;
    xcb_window_t ownerWindow = XCB_NONE;

    bool popupActive = false;
    bool pointerGrabbed = false;
    bool keyboardGrabbed = false;

    // Popups only: null in every other window.
    std::shared_ptr<X11PopupDismissal> dismissal;
};
} // namespace

std::unique_ptr<LinuxWindowNative> makeX11WindowNative(const WindowOptions& options,
                                                       WindowEvents& events)
{
    return std::make_unique<X11WindowNative>(options, events);
}
} // namespace eacp::Graphics
