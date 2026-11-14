#include "NativeChildSurface.h"

#include "LinuxWindowSurface-Linux.h"
#include "X11Connection-Linux.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>

// The container is an X11 window, because what a Linux plugin format hands an
// editor to parent into is an X11 id (VST3's kPlatformTypeX11EmbedWindowID,
// CLAP's "x11"), and nothing foreign can be parented into a Wayland surface. So
// it exists only while the view is in an X11-backed window: a toplevel on the
// X11 backend, or an EmbeddedView. In a Wayland window there is no handle.
//
// Unlike a presenting view's child window, the container is never destroyed on
// the way: a client is parented into it, and destroying a window destroys its
// children with it. Hidden is unmapped, and a view leaving its window parks the
// container, unmapped, on the root until it is placed in a window again.
//
// The client is spoken to in XEmbed: the embedder sizes it, maps it as its
// _XEMBED_INFO says, tells it it is embedded and gives it the focus it asks for.
//
// File-scope names carry an x11Child prefix: this is one unity TU under
// EACP_CI_BUILD.

namespace eacp::Graphics
{
namespace
{
constexpr uint32_t x11ChildXembedVersion = 0;
constexpr uint32_t x11ChildXembedMapped = 1u << 0;

constexpr uint32_t x11ChildXembedEmbeddedNotify = 0;
constexpr uint32_t x11ChildXembedRequestFocus = 3;
constexpr uint32_t x11ChildXembedFocusIn = 4;
constexpr uint32_t x11ChildXembedFocusCurrent = 0;

// A press, release or motion the client leaves unselected stops here rather
// than reaching the eacp view under the surface, as a child HWND's does.
constexpr uint32_t x11ChildNoPropagateMask =
    XCB_EVENT_MASK_BUTTON_PRESS | XCB_EVENT_MASK_BUTTON_RELEASE
    | XCB_EVENT_MASK_POINTER_MOTION | XCB_EVENT_MASK_BUTTON_MOTION;

template <typename T>
const T& x11ChildEventAs(const xcb_generic_event_t& event)
{
    return *reinterpret_cast<const T*>(&event);
}

struct X11ChildPixels
{
    int x = 0;
    int y = 0;
    int width = 1;
    int height = 1;

    bool operator==(const X11ChildPixels&) const = default;
};

// Rounded outwards, as an EmbeddedView is, so no seam of the window behind
// shows along the right and bottom edges.
X11ChildPixels x11ChildPixelsOf(Point origin, Rect size, float scale)
{
    const auto left = std::floor(origin.x * scale);
    const auto top = std::floor(origin.y * scale);
    const auto right = std::ceil((origin.x + size.w) * scale);
    const auto bottom = std::ceil((origin.y + size.h) * scale);

    return {(int) left,
            (int) top,
            std::max((int) (right - left), 1),
            std::max((int) (bottom - top), 1)};
}

bool x11ChildIsShown(View& view)
{
    for (auto* node = &view; node != nullptr; node = node->getParent())
        if (!node->isVisible())
            return false;

    auto bounds = view.getLocalBounds();
    return bounds.w > 0.f && bounds.h > 0.f;
}

// A window the server already destroyed is told so before its id is cleared,
// and is no place to make a container in.
X11WindowSurface* x11ChildHostOf(View& view)
{
    auto* window = dynamic_cast<X11WindowSurface*>(linuxWindowForView(view));

    if (window == nullptr || window->getWindow() == XCB_NONE
        || window->inferiorsGone)
        return nullptr;

    return window;
}

X11Connection* x11ChildLiveConnection()
{
    auto* connection = x11Connection();

    if (connection == nullptr || !connection->isConnected())
        return nullptr;

    return connection;
}
} // namespace

struct NativeChildSurface::Native final : LinuxNativeChild
{
    explicit Native(NativeChildSurface& ownerToUse)
        : owner(ownerToUse)
    {
        linuxRegisterNativeChild(owner, *this);
    }

    ~Native() override
    {
        linuxUnregisterNativeChild(owner);
        destroyContainer();
    }

    void* getParentHandle()
    {
        sync();

        if (container == XCB_NONE)
            warnAboutWayland();

        return reinterpret_cast<void*>((uintptr_t) container);
    }

    // Asks nothing of the X server until the view is in an X11 window, so a
    // Wayland app holding one of these never opens a connection for it.
    void sync() override
    {
        auto* host = x11ChildHostOf(owner);

        if (host == nullptr)
        {
            park();
            return;
        }

        auto* connection = x11ChildLiveConnection();

        if (connection == nullptr)
        {
            forgetContainer();
            return;
        }

        if (container == XCB_NONE)
            createContainer(*connection, *host);

        place(*connection, *host);
    }

    void release() override { park(); }

    void createContainer(X11Connection& connection, X11WindowSurface& hostToUse)
    {
        auto* xcb = connection.getConnection();

        pixels = pixelsIn(hostToUse);
        container = xcb_generate_id(xcb);
        parent = hostToUse.getWindow();
        host = &hostToUse;

        // No background, so what is behind shows until the client paints.
        // SubstructureNotify is how a client arriving, leaving or dying is
        // heard, StructureNotify how the container's own death is.
        const uint32_t values[] = {XCB_BACK_PIXMAP_NONE,
                                   XCB_EVENT_MASK_STRUCTURE_NOTIFY
                                       | XCB_EVENT_MASK_SUBSTRUCTURE_NOTIFY,
                                   x11ChildNoPropagateMask};

        xcb_create_window(xcb,
                          XCB_COPY_FROM_PARENT,
                          container,
                          parent,
                          x11ClampPosition(pixels.x),
                          x11ClampPosition(pixels.y),
                          x11ClampSize(pixels.width),
                          x11ClampSize(pixels.height),
                          0,
                          XCB_WINDOW_CLASS_INPUT_OUTPUT,
                          XCB_COPY_FROM_PARENT,
                          XCB_CW_BACK_PIXMAP | XCB_CW_EVENT_MASK
                              | XCB_CW_DONT_PROPAGATE,
                          values);

        // The XI2 half of the mask above: a device event goes to the nearest
        // window that selects it, and the seat ignores a window in no map.
        connection.selectPointerEvents(container);

        connection.addEventObserver(
            this, [this](const xcb_generic_event_t& event) { handleEvent(event); });

        mapped = false;
    }

    void place(X11Connection& connection, X11WindowSurface& newHost)
    {
        auto* xcb = connection.getConnection();
        auto newPixels = pixelsIn(newHost);
        auto sizeChanged =
            newPixels.width != pixels.width || newPixels.height != pixels.height;

        if (parent != newHost.getWindow())
        {
            parent = newHost.getWindow();
            xcb_reparent_window(xcb,
                                container,
                                parent,
                                x11ClampPosition(newPixels.x),
                                x11ClampPosition(newPixels.y));
        }

        host = &newHost;

        if (newPixels != pixels)
        {
            pixels = newPixels;

            const uint32_t values[] = {
                (uint32_t) (int32_t) x11ClampPosition(pixels.x),
                (uint32_t) (int32_t) x11ClampPosition(pixels.y),
                (uint32_t) x11ClampSize(pixels.width),
                (uint32_t) x11ClampSize(pixels.height)};

            xcb_configure_window(xcb,
                                 container,
                                 XCB_CONFIG_WINDOW_X | XCB_CONFIG_WINDOW_Y
                                     | XCB_CONFIG_WINDOW_WIDTH
                                     | XCB_CONFIG_WINDOW_HEIGHT,
                                 values);
        }

        if (sizeChanged)
            for (const auto& client: clients)
                fillContainer(connection, client.window);

        setMapped(connection, x11ChildIsShown(owner));

        connection.flush();
    }

    void setMapped(X11Connection& connection, bool shouldBeMapped)
    {
        if (mapped == shouldBeMapped)
            return;

        mapped = shouldBeMapped;

        if (mapped)
            xcb_map_window(connection.getConnection(), container);
        else
            xcb_unmap_window(connection.getConnection(), container);
    }

    // Out of every window and onto the root, unmapped first so it never shows
    // there: the window it was in is going away, or the view left it, and the
    // client inside must outlive both.
    void park()
    {
        if (container == XCB_NONE || host == nullptr)
            return;

        auto* connection = x11ChildLiveConnection();

        if (connection == nullptr || hostTookUsDown())
        {
            forgetContainer();
            return;
        }

        auto* xcb = connection->getConnection();

        setMapped(*connection, false);
        xcb_reparent_window(xcb, container, connection->getScreen()->root, 0, 0);
        connection->flush();

        // Parked: the next sync in a window reparents into it.
        parent = connection->getScreen()->root;
        host = nullptr;
    }

    // A client still inside is let go of first, as XEmbed says an embedder
    // ends: unmapped and handed to the root. A host that detached its plugin
    // first (a VST3 removed()) has left nothing to let go of.
    void destroyContainer()
    {
        auto* connection = x11ChildLiveConnection();

        if (connection != nullptr)
            connection->removeEventObserver(this);

        if (container == XCB_NONE || connection == nullptr || hostTookUsDown())
        {
            forgetContainer();
            return;
        }

        auto* xcb = connection->getConnection();
        auto root = connection->getScreen()->root;

        if (auto* tree =
                xcb_query_tree_reply(xcb, xcb_query_tree(xcb, container), nullptr))
        {
            const auto* children = xcb_query_tree_children(tree);
            const auto count = xcb_query_tree_children_length(tree);

            for (auto i = 0; i < count; ++i)
            {
                xcb_unmap_window(xcb, children[i]);
                xcb_reparent_window(xcb, children[i], root, 0, 0);
            }

            std::free(tree);
        }

        xcb_destroy_window(xcb, container);
        connection->flush();

        forgetContainer();
    }

    // The server destroyed the window the container was in, and the container
    // and its client with it: there is nothing left to ask it to move.
    bool hostTookUsDown() const { return host != nullptr && host->inferiorsGone; }

    void forgetContainer()
    {
        if (container != XCB_NONE)
            if (auto* connection = x11Connection())
                connection->removeEventObserver(this);

        container = XCB_NONE;
        parent = XCB_NONE;
        host = nullptr;
        mapped = false;
        pixels = {};
        clients.clear();
    }

    X11ChildPixels pixelsIn(const X11WindowSurface& window) const
    {
        return x11ChildPixelsOf(
            linuxViewOriginInWindow(owner), owner.getLocalBounds(), window.scale);
    }

    void handleEvent(const xcb_generic_event_t& event)
    {
        switch (event.response_type & ~0x80)
        {
            case XCB_CREATE_NOTIFY:
            {
                const auto& created =
                    x11ChildEventAs<xcb_create_notify_event_t>(event);

                if (created.parent == container)
                    addClient(created.window);
                break;
            }

            case XCB_REPARENT_NOTIFY:
            {
                const auto& moved =
                    x11ChildEventAs<xcb_reparent_notify_event_t>(event);

                if (moved.window == container)
                    break;

                if (moved.parent == container)
                    addClient(moved.window);
                else
                    removeClient(moved.window, true);
                break;
            }

            case XCB_DESTROY_NOTIFY:
            {
                const auto& destroyed =
                    x11ChildEventAs<xcb_destroy_notify_event_t>(event);

                if (destroyed.window == container)
                    forgetContainer();
                else
                    removeClient(destroyed.window, false);
                break;
            }

            case XCB_PROPERTY_NOTIFY:
            {
                const auto& changed =
                    x11ChildEventAs<xcb_property_notify_event_t>(event);

                if (auto* connection = x11ChildLiveConnection();
                    connection != nullptr
                    && changed.atom == connection->getAtoms().xembedInfo
                    && findClient(changed.window) != nullptr)
                {
                    applyXembedInfo(*connection, changed.window);
                    connection->flush();
                }
                break;
            }

            case XCB_CLIENT_MESSAGE:
                clientMessage(x11ChildEventAs<xcb_client_message_event_t>(event));
                break;

            default:
                break;
        }
    }

    struct Client
    {
        xcb_window_t window = XCB_NONE;
        bool xembed = false;
    };

    Client* findClient(xcb_window_t window)
    {
        for (auto& client: clients)
            if (client.window == window)
                return &client;

        return nullptr;
    }

    void addClient(xcb_window_t window)
    {
        auto* connection = x11ChildLiveConnection();

        if (connection == nullptr || window == XCB_NONE
            || findClient(window) != nullptr)
            return;

        clients.add({window, false});

        // A window of this copy's already selects what it needs, and would
        // lose it to this.
        if (connection->findWindow(window).windowSurface == nullptr)
        {
            const uint32_t mask = XCB_EVENT_MASK_PROPERTY_CHANGE;
            xcb_change_window_attributes(
                connection->getConnection(), window, XCB_CW_EVENT_MASK, &mask);
        }

        fillContainer(*connection, window);

        // After the selection, so a change made in between is not missed.
        if (applyXembedInfo(*connection, window))
            sendXembed(*connection,
                       window,
                       x11ChildXembedEmbeddedNotify,
                       0,
                       container,
                       x11ChildXembedVersion);

        connection->flush();
    }

    void removeClient(xcb_window_t window, bool stillExists)
    {
        if (findClient(window) == nullptr)
            return;

        clients.removeIndexesMatching([window](const Client& client)
                                      { return client.window == window; });

        auto* connection = x11ChildLiveConnection();

        if (!stillExists || connection == nullptr
            || connection->findWindow(window).windowSurface != nullptr)
            return;

        const uint32_t none = XCB_EVENT_MASK_NO_EVENT;
        xcb_change_window_attributes(
            connection->getConnection(), window, XCB_CW_EVENT_MASK, &none);
        connection->flush();
    }

    // The embedder decides the client's size, which is the container's.
    void fillContainer(X11Connection& connection, xcb_window_t window)
    {
        const uint32_t values[] = {0,
                                   0,
                                   (uint32_t) x11ClampSize(pixels.width),
                                   (uint32_t) x11ClampSize(pixels.height)};

        xcb_configure_window(connection.getConnection(),
                             window,
                             XCB_CONFIG_WINDOW_X | XCB_CONFIG_WINDOW_Y
                                 | XCB_CONFIG_WINDOW_WIDTH
                                 | XCB_CONFIG_WINDOW_HEIGHT,
                             values);
    }

    // True when the client speaks XEmbed. Its mapped flag is then the
    // embedder's to act on; a client without one maps itself.
    bool applyXembedInfo(X11Connection& connection, xcb_window_t window)
    {
        auto* client = findClient(window);

        if (client == nullptr)
            return false;

        auto* xcb = connection.getConnection();
        const auto atom = connection.getAtoms().xembedInfo;

        auto* reply = xcb_get_property_reply(
            xcb,
            xcb_get_property(xcb, 0, window, atom, XCB_GET_PROPERTY_TYPE_ANY, 0, 2),
            nullptr);

        if (reply == nullptr)
            return false;

        const auto valid =
            reply->format == 32 && xcb_get_property_value_length(reply) >= 8;
        const auto flags =
            valid ? static_cast<const uint32_t*>(xcb_get_property_value(reply))[1]
                  : 0u;

        std::free(reply);

        if (!valid)
            return false;

        client->xembed = true;

        if ((flags & x11ChildXembedMapped) != 0)
            xcb_map_window(xcb, window);
        else
            xcb_unmap_window(xcb, window);

        return true;
    }

    void clientMessage(const xcb_client_message_event_t& message)
    {
        auto* connection = x11ChildLiveConnection();

        if (connection == nullptr || message.window != container
            || message.type != connection->getAtoms().xembed || message.format != 32)
            return;

        if (message.data.data32[1] != x11ChildXembedRequestFocus)
            return;

        for (const auto& client: clients)
        {
            if (!client.xembed)
                continue;

            xcb_set_input_focus(connection->getConnection(),
                                XCB_INPUT_FOCUS_PARENT,
                                client.window,
                                XCB_CURRENT_TIME);
            sendXembed(*connection,
                       client.window,
                       x11ChildXembedFocusIn,
                       x11ChildXembedFocusCurrent,
                       0,
                       0);
        }

        connection->flush();
    }

    void sendXembed(X11Connection& connection,
                    xcb_window_t window,
                    uint32_t opcode,
                    uint32_t detail,
                    uint32_t data1,
                    uint32_t data2)
    {
        auto message = xcb_client_message_event_t {};
        message.response_type = XCB_CLIENT_MESSAGE;
        message.format = 32;
        message.window = window;
        message.type = connection.getAtoms().xembed;
        message.data.data32[0] = XCB_CURRENT_TIME;
        message.data.data32[1] = opcode;
        message.data.data32[2] = detail;
        message.data.data32[3] = data1;
        message.data.data32[4] = data2;

        xcb_send_event(connection.getConnection(),
                       0,
                       window,
                       XCB_EVENT_MASK_NO_EVENT,
                       reinterpret_cast<const char*>(&message));
    }

    // Once per surface, and only for the case a host can do something about.
    void warnAboutWayland()
    {
        auto* window = linuxWindowForView(owner);

        if (warnedAboutWayland || window == nullptr
            || window->nativeSurface.kind != NativeSurfaceHandle::Kind::Wayland)
            return;

        warnedAboutWayland = true;

        LOG("NativeChildSurface: this view is in a Wayland window, which nothing "
            "X11 can be parented into, so it has no handle. A host showing X11 "
            "editors runs on the X11 backend: EACP_WINDOW_SYSTEM=x11.");
    }

    NativeChildSurface& owner;

    xcb_window_t container = XCB_NONE;

    // The window the container is in: the host's, the root while parked, and
    // none while there is no container. The host is null while parked.
    xcb_window_t parent = XCB_NONE;
    X11WindowSurface* host = nullptr;

    X11ChildPixels pixels;
    bool mapped = false;
    bool warnedAboutWayland = false;

    Vector<Client> clients;
};

NativeChildSurface::NativeChildSurface()
    : impl(*this)
{
}

NativeChildSurface::~NativeChildSurface() = default;

void* NativeChildSurface::getNativeParentHandle()
{
    return impl->getParentHandle();
}

void NativeChildSurface::refreshPlacement()
{
    impl->sync();
}

void NativeChildSurface::resized()
{
    View::resized();
    refreshPlacement();
}

void NativeChildSurface::visibilityChanged(bool)
{
    impl->sync();
}

} // namespace eacp::Graphics
