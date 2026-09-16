#include "EmbeddedView.h"

#include "../View/View.h"
#include "X11Connection-Linux.h"
#include "X11Input-Linux.h"

#include <eacp/Core/Threads/EventLoop.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>

// An embedded surface is always X11, whatever window system this copy prefers:
// the id the host handed over is one, and every plugin API on Linux hands out
// an X11 id (plan.md D6).
//
// File-scope names carry an x11/X11 prefix: this is one unity TU under
// EACP_CI_BUILD.

namespace eacp::Graphics
{
namespace
{
template <typename T>
const T& x11EmbeddedAs(const xcb_generic_event_t& event)
{
    return *reinterpret_cast<const T*>(&event);
}

// What the server measures the child in, which is not what the surface is
// placed in.
struct X11PixelRect
{
    int x = 0;
    int y = 0;
    int width = 1;
    int height = 1;

    bool operator==(const X11PixelRect&) const = default;
};

// Grown to the smallest pixel rect containing the points rather than rounded
// to the nearest, for the reason on EmbeddedView::setBounds: a surface rounded
// down leaves a seam of the host's own window showing along its right and
// bottom edges.
X11PixelRect x11PixelsOf(const Rect& bounds, float scale)
{
    const auto left = std::floor(bounds.x * scale);
    const auto top = std::floor(bounds.y * scale);
    const auto right = std::ceil((bounds.x + bounds.w) * scale);
    const auto bottom = std::ceil((bounds.y + bounds.h) * scale);

    return {(int) left,
            (int) top,
            std::max((int) (right - left), 1),
            std::max((int) (bottom - top), 1)};
}

void x11NotifyEmbeddedVisibility(View* view, bool visible)
{
    if (view == nullptr)
        return;

    view->hostWindowVisibilityChanged(visible);

    for (auto* child: view->getSubviews())
        x11NotifyEmbeddedVisibility(child, visible);
}
} // namespace

// An InputOutput child of the host's window, selecting what a toplevel of ours
// selects, so the seat reaches the content view through the same X11Input and
// a presenting view gets the same child window under it. Not a
// LinuxWindowNative: there is no Window above this, no options and no
// WindowEvents, and what a toplevel decides for itself the host decides here.
struct EmbeddedView::Native final : X11WindowSurface
{
    Native(void* hostParentHandle, const Rect& initialBounds)
        // The host's id came in widened into the pointer parameter every
        // plugin API passes one in; this is the same trip back.
        : parent((xcb_window_t) reinterpret_cast<uintptr_t>(hostParentHandle))
        , bounds(initialBounds)
    {
        // As on Windows, and before anything defers work: the thread the host
        // built the surface on is the one every callback has to come back to,
        // and on Linux nothing else is going to say so.
        Threads::attachCurrentThreadAsMain();

        onConnectionLost = [this] { markWindowGone(); };

        pixels = x11PixelsOf(bounds, scale);

        // Before any window, and whether or not there is ever going to be
        // one: a surface with no server to reach still lays its content out
        // at the size it was asked for, exactly as a headless toplevel does.
        updateContentSize();

        createWindow();
    }

    // The content view goes first, while the window id it was drawn against is
    // still live; after that the child is nobody's.
    ~Native() override
    {
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

    // No connection, no display, or a host that handed over nothing: the
    // surface is the one a headless build gets - remembered, laid out, and
    // shown nowhere.
    void createWindow()
    {
        auto* connection = x11Connection();

        if (connection == nullptr || !connection->isConnected()
            || parent == XCB_NONE)
            return;

        auto* xcb = connection->getConnection();

        readParentGeometry();

        auto window = xcb_generate_id(xcb);

        // No background pixmap: what the host painted stays until our own
        // content is there, rather than a flash of a colour we invented. The
        // depth and visual are the host's, because they are the only ones the
        // child may be a child of.
        const uint32_t values[] = {XCB_BACK_PIXMAP_NONE,
                                   connection->getWindowEventMask()};

        xcb_create_window(xcb,
                          XCB_COPY_FROM_PARENT,
                          window,
                          parent,
                          x11ClampPosition(pixels.x),
                          x11ClampPosition(pixels.y),
                          x11ClampSize(pixels.width),
                          x11ClampSize(pixels.height),
                          0,
                          XCB_WINDOW_CLASS_INPUT_OUTPUT,
                          XCB_COPY_FROM_PARENT,
                          XCB_CW_BACK_PIXMAP | XCB_CW_EVENT_MASK,
                          values);

        setWindow(window);
        connection->registerWindow({window, this, nullptr});
        connection->selectPointerEvents(window);

        watchParent();
        updateContentSize();

        // Shown from the start, as the AppKit and Win32 surfaces are: a host
        // that wants it hidden says so, and mapped waits for the MapNotify.
        xcb_map_window(xcb, window);

        connection->flush();
    }

    // The host's window is somebody else's, so the only thing to do to it is
    // listen: an event mask belongs to the client that selected it, and
    // StructureNotify is what says the host resized.
    void watchParent()
    {
        auto* connection = liveConnection();

        if (connection == nullptr || parent == XCB_NONE)
            return;

        // Unless the host is a window of this copy's, whose own mask this
        // would replace - it is already routed, and the watch alone is what
        // brings its events here as well. Every window in that map but one
        // selects the same mask this would leave behind anyway; the exception
        // is a view's child window, which selects Exposure alone and which
        // nothing hands out as a host id.
        if (connection->findWindow(parent).windowSurface == nullptr)
        {
            const uint32_t mask = XCB_EVENT_MASK_STRUCTURE_NOTIFY;

            xcb_change_window_attributes(
                connection->getConnection(), parent, XCB_CW_EVENT_MASK, &mask);
        }

        connection->watchForeignWindow({parent, this, nullptr});
    }

    // The size the surface fills until somebody places it. A host that cannot
    // be read leaves the options' size standing.
    void readParentGeometry()
    {
        auto* connection = liveConnection();

        if (connection == nullptr || !followingHost)
            return;

        auto* xcb = connection->getConnection();

        auto* reply =
            xcb_get_geometry_reply(xcb, xcb_get_geometry(xcb, parent), nullptr);

        if (reply == nullptr)
            return;

        const auto width = (int) reply->width;
        const auto height = (int) reply->height;
        std::free(reply);

        takePixelSize(width, height);
    }

    // The connection, and only while this child is a live id on it.
    X11Connection* liveConnection() const
    {
        auto* connection = x11Connection();

        if (connection == nullptr || !connection->isConnected())
            return nullptr;

        return connection;
    }

    bool isOurWindow(xcb_window_t window) const
    {
        return window != XCB_NONE && window == getWindow();
    }

    // The seat, and only for an event this surface's own window was named in:
    // where the host is a window of this copy's, its events reach here too,
    // and those are not this surface's.
    X11Input* seatFor(xcb_window_t eventWindow) const
    {
        return isOurWindow(eventWindow) ? seat() : nullptr;
    }

    void handleEvent(const xcb_generic_event_t& event) override
    {
        switch (event.response_type & ~0x80)
        {
            case XCB_MAP_NOTIFY:
                mapNotify(x11EmbeddedAs<xcb_map_notify_event_t>(event));
                break;

            case XCB_UNMAP_NOTIFY:
                unmapNotify(x11EmbeddedAs<xcb_unmap_notify_event_t>(event));
                break;

            case XCB_CONFIGURE_NOTIFY:
                configureNotify(x11EmbeddedAs<xcb_configure_notify_event_t>(event));
                break;

            case XCB_REPARENT_NOTIFY:
                reparentNotify(x11EmbeddedAs<xcb_reparent_notify_event_t>(event));
                break;

            case XCB_DESTROY_NOTIFY:
                destroyNotify(x11EmbeddedAs<xcb_destroy_notify_event_t>(event));
                break;

            case XCB_EXPOSE:
                expose(x11EmbeddedAs<xcb_expose_event_t>(event));
                break;

            case XCB_FOCUS_IN:
                focusChanged(x11EmbeddedAs<xcb_focus_in_event_t>(event), true);
                break;

            case XCB_FOCUS_OUT:
                focusChanged(x11EmbeddedAs<xcb_focus_out_event_t>(event), false);
                break;

            // Everything the seat says, with nothing of ours to add: an event
            // selected on the child arrives in the child's own coordinates,
            // and a view of ours is the only thing under it.
            case XCB_KEY_PRESS:
            case XCB_KEY_RELEASE:
            {
                const auto& key = x11EmbeddedAs<xcb_key_press_event_t>(event);

                if (auto* seatInput = seatFor(key.event))
                    seatInput->keyChanged(
                        *this, key, (event.response_type & ~0x80) == XCB_KEY_PRESS);
                break;
            }

            case XCB_BUTTON_PRESS:
            case XCB_BUTTON_RELEASE:
            {
                const auto& button = x11EmbeddedAs<xcb_button_press_event_t>(event);

                if (auto* seatInput = seatFor(button.event))
                    seatInput->buttonChanged(*this,
                                             button,
                                             (event.response_type & ~0x80)
                                                 == XCB_BUTTON_PRESS);
                break;
            }

            case XCB_MOTION_NOTIFY:
            {
                const auto& motion = x11EmbeddedAs<xcb_motion_notify_event_t>(event);

                if (auto* seatInput = seatFor(motion.event))
                    seatInput->pointerMoved(*this, motion);
                break;
            }

            case XCB_ENTER_NOTIFY:
            {
                const auto& entered = x11EmbeddedAs<xcb_enter_notify_event_t>(event);

                if (auto* seatInput = seatFor(entered.event))
                    seatInput->pointerEntered(*this, entered);
                break;
            }

            case XCB_LEAVE_NOTIFY:
            {
                const auto& left = x11EmbeddedAs<xcb_leave_notify_event_t>(event);

                if (auto* seatInput = seatFor(left.event))
                    seatInput->pointerLeft(*this, left);
                break;
            }

            default:
                break;
        }
    }

    void mapNotify(const xcb_map_notify_event_t& event)
    {
        if (!isOurWindow(event.window) || mapped)
            return;

        mapped = true;

        contentStateChanged();
        x11NotifyEmbeddedVisibility(contentView, true);
    }

    void unmapNotify(const xcb_unmap_notify_event_t& event)
    {
        if (!isOurWindow(event.window) || !mapped)
            return;

        mapped = false;

        contentStateChanged();
        x11NotifyEmbeddedVisibility(contentView, false);
    }

    // The host resized, which the surface follows until it is placed; or the
    // host resized the child itself, which some do instead, and that size is
    // taken whether or not anything is following, because arguing with the
    // window the host put us in is not this surface's place.
    void configureNotify(const xcb_configure_notify_event_t& event)
    {
        if (event.window == parent)
        {
            if (followingHost)
                takeParentSize((int) event.width, (int) event.height);

            return;
        }

        if (isOurWindow(event.window))
            adoptChildGeometry({(int) event.x,
                                (int) event.y,
                                std::max((int) event.width, 1),
                                std::max((int) event.height, 1)});
    }

    // The host moved the surface into another window of its own, which is a
    // host with a layout rearranging itself. The new parent is the one to
    // follow from here.
    void reparentNotify(const xcb_reparent_notify_event_t& event)
    {
        if (!isOurWindow(event.window) || event.parent == parent)
            return;

        if (auto* connection = x11Connection())
            connection->unwatchForeignWindow(*this);

        parent = event.parent;

        watchParent();

        // A following surface fills whatever it is now inside; a placed one
        // stays where the host put it, and says so to the new parent.
        readParentGeometry();
        applyGeometry();
    }

    // The child is gone with whatever took it - the host destroying its own
    // window takes ours down with it, and the server says so about the child
    // as well as about the host - so nothing more may be sent to the id. Only
    // the child's own notice is acted on: a host id is somebody else's and the
    // server hands one out again as soon as the client that had it goes.
    void destroyNotify(const xcb_destroy_notify_event_t& event)
    {
        if (!isOurWindow(event.window))
            return;

        if (auto* connection = x11Connection())
        {
            connection->unwatchForeignWindow(*this);

            if (getWindow() != XCB_NONE)
                connection->unregisterWindow(getWindow());
        }

        // Before the view surfaces are told: their own windows were inside
        // this one and went with it.
        inferiorsGone = true;

        markWindowGone();
    }

    void expose(const xcb_expose_event_t& event)
    {
        // An exposed region arrives in pieces; only the last is worth a
        // repaint of the whole surface.
        if (event.count != 0)
            return;

        auto* connection = x11Connection();

        if (connection == nullptr)
            return;

        // A presenting view's own child window; the surface itself has no
        // background for the server to ask us about.
        if (auto* view = connection->findWindow(event.window).view)
            view->repaint();
    }

    void focusChanged(const xcb_focus_in_event_t& event, bool focused)
    {
        if (!isOurWindow(event.event))
            return;

        if (auto* seatInput = seat())
            seatInput->focusChanged(*this, event, focused);
    }

    void setContentView(View& view)
    {
        if (contentView == &view)
            return;

        if (contentView != nullptr)
            linuxUnbindWindowFromContentView(*contentView);

        contentView = &view;

        view.setBounds({0.f, 0.f, contentSize.x, contentSize.y});

        linuxBindWindowToContentView(view, *this);
    }

    void setVisible(bool shouldBeVisible)
    {
        auto* connection = liveConnection();

        if (connection == nullptr || getWindow() == XCB_NONE)
            return;

        if (shouldBeVisible)
        {
            xcb_map_window(connection->getConnection(), getWindow());
            connection->flush();
            return;
        }

        // Eagerly, unlike the map: the view surfaces have to go before the
        // request reaches the server, and the UnmapNotify finds it done.
        if (mapped)
        {
            mapped = false;

            contentStateChanged();
            x11NotifyEmbeddedVisibility(contentView, false);
        }

        xcb_unmap_window(connection->getConnection(), getWindow());
        connection->flush();
    }

    // The host is the authority on where the surface goes from here, so the
    // parent's size stops being an answer to anything.
    void stopFollowingHost() { followingHost = false; }

    void place(const Rect& newBounds)
    {
        bounds = newBounds;
        pixels = x11PixelsOf(bounds, scale);

        applyGeometry();
    }

    // X11 has no per-window scale to read, so one pixel per point is the
    // platform's own answer and the host's is the only other one there is.
    // Everything measured from it is re-derived: the pixels the surface covers
    // where it was placed, and the points its content is laid out in either
    // way.
    void setScale(float pixelsPerPoint)
    {
        scale = pixelsPerPoint > 0.f ? pixelsPerPoint : linuxDefaultBackingScale;

        if (followingHost)
            bounds = {bounds.x,
                      bounds.y,
                      (float) pixels.width / scale,
                      (float) pixels.height / scale};
        else
            pixels = x11PixelsOf(bounds, scale);

        applyGeometry();
    }

    void takeParentSize(int width, int height)
    {
        if (!takePixelSize(width, height))
            return;

        applyGeometry();
    }

    // True when anything moved. The surface fills the host from its top left,
    // so a parent's size is the whole of the child's geometry.
    bool takePixelSize(int width, int height)
    {
        const auto wanted =
            X11PixelRect {0, 0, std::max(width, 1), std::max(height, 1)};

        if (wanted == pixels)
            return false;

        pixels = wanted;
        bounds = {
            0.f, 0.f, (float) pixels.width / scale, (float) pixels.height / scale};

        return true;
    }

    // A geometry the server has already given the child - a host that places
    // the plugin's window itself rather than resizing its own - taken rather
    // than asked for again, so the next applyGeometry does not snap the child
    // back to where this surface last put it.
    void adoptChildGeometry(const X11PixelRect& given)
    {
        if (given == pixels)
            return;

        pixels = given;

        bounds = {(float) pixels.x / scale,
                  (float) pixels.y / scale,
                  (float) pixels.width / scale,
                  (float) pixels.height / scale};

        updateContentSize();
    }

    void applyGeometry()
    {
        if (auto* connection = liveConnection();
            connection != nullptr && getWindow() != XCB_NONE)
        {
            const uint32_t values[] = {
                (uint32_t) (int32_t) x11ClampPosition(pixels.x),
                (uint32_t) (int32_t) x11ClampPosition(pixels.y),
                (uint32_t) x11ClampSize(pixels.width),
                (uint32_t) x11ClampSize(pixels.height)};

            xcb_configure_window(connection->getConnection(),
                                 getWindow(),
                                 XCB_CONFIG_WINDOW_X | XCB_CONFIG_WINDOW_Y
                                     | XCB_CONFIG_WINDOW_WIDTH
                                     | XCB_CONFIG_WINDOW_HEIGHT,
                                 values);
            connection->flush();
        }

        updateContentSize();
    }

    // The content is laid out in points, and the points are what is left of
    // the pixels the host gave once the scale it named is taken out.
    void updateContentSize()
    {
        contentSize = {(float) pixels.width / scale, (float) pixels.height / scale};

        if (contentView == nullptr)
            return;

        contentView->setBounds({0.f, 0.f, contentSize.x, contentSize.y});

        linuxWindowSurfaceStateChanged(*contentView);
    }

    void contentStateChanged()
    {
        if (contentView != nullptr)
            linuxWindowSurfaceStateChanged(*contentView);
    }

    // Whichever way the id went, what is left is the surface a headless build
    // has. The view surfaces' onLost has to fire while their own ids are still
    // the ones the records hold, so the state change goes first.
    void markWindowGone()
    {
        auto wasMapped = mapped;
        mapped = false;

        contentStateChanged();

        if (auto* seatInput = seat())
            seatInput->windowDestroyed(*this);

        setWindow(XCB_NONE);

        if (wasMapped)
            x11NotifyEmbeddedVisibility(contentView, false);
    }

    static X11Input* seat()
    {
        auto* connection = x11Connection();

        return connection != nullptr ? connection->getInput() : nullptr;
    }

    xcb_window_t parent = XCB_NONE;

    // In points, and the surface's own copy: EmbeddedView's is what the host
    // last set, while this one follows the host until it does.
    Rect bounds;

    X11PixelRect pixels;

    bool followingHost = true;
};

EmbeddedView::EmbeddedView(void* hostParentHandle,
                           const EmbeddedViewOptions& optionsToUse)
    : bounds(0.f, 0.f, (float) optionsToUse.width, (float) optionsToUse.height)
    , impl(hostParentHandle, bounds)
{
}

EmbeddedView::~EmbeddedView() = default;

void EmbeddedView::setContentView(View& view)
{
    impl->setContentView(view);
}

void EmbeddedView::setBounds(const Rect& newBounds)
{
    bounds = newBounds;

    impl->stopFollowingHost();
    impl->place(bounds);
}

void EmbeddedView::setSize(int width, int height)
{
    bounds = bounds.withSize((float) width, (float) height);
    impl->place(bounds);
}

void EmbeddedView::setVisible(bool shouldBeVisible)
{
    visible = shouldBeVisible;
    impl->setVisible(shouldBeVisible);
}

void EmbeddedView::setPixelsPerPoint(float pixelsPerPoint)
{
    impl->setScale(pixelsPerPoint);
}

// The xcb_window_t widened into a pointer, which is how the id came in and how
// every plugin API passes one. Null while there is no window: no display, no
// host id, or a connection that has gone.
void* EmbeddedView::getHandle()
{
    return reinterpret_cast<void*>((uintptr_t) impl->getWindow());
}
} // namespace eacp::Graphics
