#include "Window.h"

#include "../View/WaylandViewSurface-Linux.h"
#include "WaylandDisplay-Linux.h"
#include "WaylandInput-Linux.h"

#include <algorithm>
#include <cmath>

namespace eacp::Graphics
{
namespace
{
// Names a .desktop file; the protocol has no per-window icon.
constexpr const char* waylandDefaultAppId = "eacp";

constexpr Color waylandDefaultWindowBackground = Color::gray(0.f);

void waylandNotifyHostVisibility(View* view, bool visible)
{
    if (view == nullptr)
        return;

    view->hostWindowVisibilityChanged(visible);

    for (auto* child: view->getSubviews())
        waylandNotifyHostVisibility(child, visible);
}

bool waylandFlagSet(const WindowOptions& options, WindowFlags flag)
{
    return options.flags.contains(flag);
}
} // namespace

struct Window::Native : WaylandWindowSurface
{
    Native(const WindowOptions& optionsToUse, WindowEvents& eventsToUse)
        : title(optionsToUse.title)
        , quitCallback(optionsToUse.effectiveOnQuit())
        , onResize(optionsToUse.onResize)
        , onWillResize(optionsToUse.onWillResize)
        , events(&eventsToUse)
        , minWidth(optionsToUse.minWidth)
        , minHeight(optionsToUse.minHeight)
        , aspectRatio(optionsToUse.hasAspectRatio() ? optionsToUse.aspectRatio
                                                    : std::optional<Point> {})
        , hidesOnClose(optionsToUse.hidesOnClose)
        , resizable(waylandFlagSet(optionsToUse, WindowFlags::Resizable))
        , closable(waylandFlagSet(optionsToUse, WindowFlags::Closable))
        , miniaturizable(waylandFlagSet(optionsToUse, WindowFlags::Miniaturizable))
        , transparent(optionsToUse.transparentBackground)
        , background(
              optionsToUse.backgroundColor.value_or(waylandDefaultWindowBackground))
    {
        contentSize = {(float) optionsToUse.width, (float) optionsToUse.height};

        if (optionsToUse.initialPosition)
            position = *optionsToUse.initialPosition;

        if (transparent)
            background = Color {0.f, 0.f, 0.f, 0.f};

        onKeyboardFocus = [this](bool focused) { keyboardFocusChanged(focused); };
        onConnectionLost = [this] { connectionLost(); };

        createSurface();
    }

    ~Native()
    {
        if (contentView != nullptr)
            waylandUnbindWindowFromContentView(*contentView);

        destroyFrame();

        if (auto* connection = waylandDisplay())
        {
            if (auto* seatInput = connection->getInput())
                seatInput->windowDestroyed(*this);

            if (surface != nullptr)
                connection->unregisterSurface(surface);
        }

        buffer.destroy();

        if (fractionalScale != nullptr)
            wp_fractional_scale_v1_destroy(fractionalScale);

        if (viewport != nullptr)
            wp_viewport_destroy(viewport);

        if (surface != nullptr)
            wl_surface_destroy(surface);
    }

    void createSurface()
    {
        auto* connection = waylandDisplay();

        if (connection == nullptr || connection->getCompositor() == nullptr)
            return;

        surface = wl_compositor_create_surface(connection->getCompositor());

        if (surface == nullptr)
            return;

        wl_surface_add_listener(surface, &surfaceListener(), this);
        connection->registerSurface({surface, this, nullptr});

        if (auto* viewporter = connection->getViewporter())
            viewport = wp_viewporter_get_viewport(viewporter, surface);

        if (auto* scales = connection->getFractionalScales())
        {
            fractionalScale =
                wp_fractional_scale_manager_v1_get_fractional_scale(scales, surface);
            wp_fractional_scale_v1_add_listener(
                fractionalScale, &fractionalScaleListener(), this);
        }

        scale = connection->getFallbackScale();
    }

    void createFrame()
    {
        auto* connection = waylandDisplay();

        if (frame != nullptr || surface == nullptr || connection == nullptr)
            return;

        auto* decorations = connection->getDecorations();

        if (decorations == nullptr)
            return;

        frame = libdecor_decorate(decorations, surface, &frameListener(), this);

        if (frame == nullptr)
            return;

        libdecor_frame_set_title(frame, title.c_str());
        libdecor_frame_set_app_id(frame, waylandDefaultAppId);

        if (minWidth > 0 || minHeight > 0)
            libdecor_frame_set_min_content_size(
                frame, std::max(minWidth, 1), std::max(minHeight, 1));

        // Pinned as well as unset: a compositor may configure a size no client
        // asked for.
        if (!resizable)
        {
            libdecor_frame_unset_capabilities(
                frame,
                (enum libdecor_capabilities)(LIBDECOR_ACTION_RESIZE
                                             | LIBDECOR_ACTION_FULLSCREEN));

            auto width = std::max((int) std::lround(contentSize.x), 1);
            auto height = std::max((int) std::lround(contentSize.y), 1);

            libdecor_frame_set_min_content_size(frame, width, height);
            libdecor_frame_set_max_content_size(frame, width, height);
        }

        if (!closable)
            libdecor_frame_unset_capabilities(frame, LIBDECOR_ACTION_CLOSE);

        if (!miniaturizable)
            libdecor_frame_unset_capabilities(frame, LIBDECOR_ACTION_MINIMIZE);

        libdecor_frame_map(frame);
        connection->flush();
    }

    void destroyFrame()
    {
        if (frame == nullptr)
            return;

        libdecor_frame_unref(frame);
        frame = nullptr;
    }

    void configure(libdecor_configuration* configuration)
    {
        auto width = std::max((int) std::lround(contentSize.x), 1);
        auto height = std::max((int) std::lround(contentSize.y), 1);

        auto proposedWidth = 0;
        auto proposedHeight = 0;

        if (libdecor_configuration_get_content_size(
                configuration, frame, &proposedWidth, &proposedHeight))
        {
            width = proposedWidth;
            height = proposedHeight;
        }

        applyConstraints(width, height);

        auto* state = libdecor_state_new(width, height);
        libdecor_frame_commit(frame, state, configuration);
        libdecor_state_free(state);

        auto windowState = LIBDECOR_WINDOW_STATE_NONE;

        if (libdecor_configuration_get_window_state(configuration, &windowState))
        {
            maximized = (windowState & LIBDECOR_WINDOW_STATE_MAXIMIZED) != 0;
            setActive((windowState & LIBDECOR_WINDOW_STATE_ACTIVE) != 0);
        }

        resizeTo({(float) width, (float) height});
        present();

        if (!mapped)
        {
            mapped = true;
            waylandNotifyHostVisibility(contentView, true);
        }

        if (contentView != nullptr)
            waylandWindowSurfaceStateChanged(*contentView);
    }

    void applyConstraints(int& width, int& height) const
    {
        if (onWillResize)
            onWillResize(width, height);

        if (aspectRatio)
        {
            // Width drives: libdecor's configuration carries no resize edge.
            const auto ratio = aspectRatio->x / aspectRatio->y;
            height = (int) std::lround((float) width / ratio);
        }

        width = std::max(width, std::max(minWidth, 1));
        height = std::max(height, std::max(minHeight, 1));
    }

    void resizeTo(Point newSize)
    {
        if (newSize.x == contentSize.x && newSize.y == contentSize.y)
            return;

        contentSize = newSize;

        if (contentView != nullptr)
            contentView->setBounds({0.f, 0.f, contentSize.x, contentSize.y});

        if (onResize)
            onResize((int) contentSize.x, (int) contentSize.y);
    }

    void present()
    {
        auto* connection = waylandDisplay();

        if (surface == nullptr || connection == nullptr)
            return;

        auto width = std::max((int) std::lround(contentSize.x), 1);
        auto height = std::max((int) std::lround(contentSize.y), 1);

        if (viewport != nullptr)
        {
            // One pixel, stretched: a resize costs a viewport request, not a
            // new shm buffer per frame of the drag.
            if (buffer.get() == nullptr)
                buffer.create(connection->getShm(), 1, 1, background);

            wp_viewport_set_destination(viewport, width, height);
        }
        else if (buffer.getWidth() != width || buffer.getHeight() != height)
        {
            buffer.create(connection->getShm(), width, height, background);
        }

        if (buffer.get() == nullptr)
            return;

        applyOpaqueRegion(width, height);

        wl_surface_attach(surface, buffer.get(), 0, 0);
        wl_surface_damage_buffer(surface, 0, 0, INT32_MAX, INT32_MAX);
        wl_surface_commit(surface);

        connection->flush();
    }

    void applyOpaqueRegion(int width, int height)
    {
        auto* connection = waylandDisplay();

        if (connection == nullptr || connection->getCompositor() == nullptr)
            return;

        if (transparent)
        {
            wl_surface_set_opaque_region(surface, nullptr);
            return;
        }

        auto* region = wl_compositor_create_region(connection->getCompositor());
        wl_region_add(region, 0, 0, width, height);
        wl_surface_set_opaque_region(surface, region);
        wl_region_destroy(region);
    }

    void setContentView(View* view)
    {
        contentView = view;

        if (contentView == nullptr)
            return;

        contentView->setBounds({0.f, 0.f, contentSize.x, contentSize.y});

        waylandBindWindowToContentView(*contentView, *this);

        if (surface != nullptr)
            setVisible(true);
    }

    void setVisible(bool shouldBeVisible)
    {
        if (surface == nullptr)
            return;

        if (shouldBeVisible)
        {
            createFrame();
            return;
        }

        if (mapped || frame != nullptr)
            unmap();
    }

    // The frame is torn down and rebuilt on the way back: an xdg_surface's
    // initial configure sequence happens only once.
    void unmap()
    {
        auto wasMapped = mapped;
        mapped = false;

        if (contentView != nullptr)
            waylandWindowSurfaceStateChanged(*contentView);

        destroyFrame();
        setActive(false);

        wl_surface_attach(surface, nullptr, 0, 0);
        wl_surface_commit(surface);
        buffer.destroy();

        if (auto* connection = waylandDisplay())
            connection->flush();

        if (wasMapped)
            waylandNotifyHostVisibility(contentView, false);
    }

    void setTitle(const std::string& newTitle)
    {
        title = newTitle;

        if (frame != nullptr)
            libdecor_frame_set_title(frame, title.c_str());
    }

    // Wayland has no global coordinates: the value put in is the one handed
    // back, and nothing asks the compositor.
    void setPosition(Point newPosition)
    {
        position = newPosition;
        events->onMoved(position);
    }

    void setActive(bool nowActive)
    {
        if (active == nowActive)
            return;

        active = nowActive;
        events->onActivationChanged(active);
    }

    void keyboardFocusChanged(bool focused) { setActive(focused); }

    // The compositor went away. Everything made from the connection is
    // dropped, including the view surfaces, whose onLost has to fire while
    // their wl_surface is still a live object; what is left is the window a
    // headless build has.
    void connectionLost()
    {
        auto wasMapped = mapped;
        mapped = false;

        if (contentView != nullptr)
            waylandWindowSurfaceStateChanged(*contentView);

        destroyFrame();
        setActive(false);
        buffer.destroy();

        if (fractionalScale != nullptr)
        {
            wp_fractional_scale_v1_destroy(fractionalScale);
            fractionalScale = nullptr;
        }

        if (viewport != nullptr)
        {
            wp_viewport_destroy(viewport);
            viewport = nullptr;
        }

        if (surface != nullptr)
        {
            wl_surface_destroy(surface);
            surface = nullptr;
        }

        if (wasMapped)
            waylandNotifyHostVisibility(contentView, false);
    }

    void closeRequested()
    {
        if (hidesOnClose)
        {
            unmap();
            events->onHidden();
            return;
        }

        quitCallback();
    }

    void scaleChanged(float newScale)
    {
        if (newScale <= 0.f || newScale == scale)
            return;

        scale = newScale;

        if (contentView == nullptr)
            return;

        // A scale change carries no size with it, so nothing else reports it.
        notifyBackingScaleChanged(*contentView);
        waylandWindowSurfaceStateChanged(*contentView);
    }

    void setMouseLocked(bool locked)
    {
        mouseLockIntent = locked;

        if (auto* connection = waylandDisplay())
            if (auto* seatInput = connection->getInput())
                seatInput->updateMouseLock(*this);
    }

    bool hasKeyboardFocus() const
    {
        auto* connection = waylandDisplay();

        if (connection == nullptr || connection->getInput() == nullptr)
            return false;

        return connection->getInput()->getKeyboardFocus() == this;
    }

    static Native& self(void* data) { return *static_cast<Native*>(data); }

    // Non-const because libdecor_decorate keeps the pointer it is handed.
    static libdecor_frame_interface& frameListener()
    {
        // Zero-initialised: the struct carries reserved slots libdecor never
        // uses.
        static auto table = []
        {
            auto built = libdecor_frame_interface {};

            built.configure = [](libdecor_frame*,
                                 libdecor_configuration* configuration,
                                 void* data)
            { self(data).configure(configuration); };

            built.close = [](libdecor_frame*, void* data)
            { self(data).closeRequested(); };

            // The decorations are on synchronous subsurfaces of ours, and
            // reach the screen only when this surface commits.
            built.commit = [](libdecor_frame*, void* data) { self(data).present(); };

            built.dismiss_popup = [](libdecor_frame*, const char*, void*) {};

            return built;
        }();

        return table;
    }

    static const wp_fractional_scale_v1_listener& fractionalScaleListener()
    {
        static const wp_fractional_scale_v1_listener table {
            .preferred_scale =
                [](void* data, wp_fractional_scale_v1*, uint32_t scale120)
            {
                // The protocol's unit is 120ths, so 1.5x arrives as 180.
                self(data).scaleChanged((float) scale120 / 120.f);
            },
        };

        return table;
    }

    static const wl_surface_listener& surfaceListener()
    {
        static const wl_surface_listener table {
            .enter =
                [](void* data, wl_surface*, wl_output*)
            {
                auto& window = self(data);

                if (window.fractionalScale == nullptr)
                    if (auto* connection = waylandDisplay())
                        window.scaleChanged(connection->getFallbackScale());
            },
            .leave = [](void*, wl_surface*, wl_output*) {},
            .preferred_buffer_scale =
                [](void* data, wl_surface*, int32_t factor)
            {
                if (self(data).fractionalScale == nullptr)
                    self(data).scaleChanged((float) std::max(factor, 1));
            },
            .preferred_buffer_transform = [](void*, wl_surface*, uint32_t) {},
        };

        return table;
    }

    std::string title;
    Callback quitCallback;
    ResizeCallback onResize;
    WillResizeCallback onWillResize;
    WindowEvents* events;

    int minWidth = 0;
    int minHeight = 0;
    std::optional<Point> aspectRatio;
    bool hidesOnClose = false;
    bool resizable = true;
    bool closable = true;
    bool miniaturizable = true;
    bool transparent = false;
    Color background;

    Point position;
    bool active = false;
    bool maximized = false;

    libdecor_frame* frame = nullptr;
    wp_viewport* viewport = nullptr;
    wp_fractional_scale_v1* fractionalScale = nullptr;
    WaylandShmBuffer buffer;
};

Window::Window(const WindowOptions& optionsToUse)
    : options(optionsToUse)
    , impl(optionsToUse, events)
{
}

Window::~Window() = default;

void Window::setTitle(const std::string& title)
{
    impl->setTitle(title);
}

// The wl_surface. Null under headless and wherever no compositor was reached.
void* Window::getHandle()
{
    return impl->surface;
}

void* Window::getContentViewHandle()
{
    return impl->contentView != nullptr ? impl->contentView->getHandle() : nullptr;
}

void Window::setContentView(View& view)
{
    contentLink.attach(&view, this);
    impl->setContentView(&view);
}

// A Wayland client cannot raise itself, so this only shows the window.
void Window::toFront()
{
    impl->setVisible(true);
}

void Window::setVisible(bool visible)
{
    impl->setVisible(visible);
}

void Window::minimize()
{
    if (impl->frame != nullptr)
        libdecor_frame_set_minimized(impl->frame);
}

void Window::toggleMaximize()
{
    if (impl->frame == nullptr)
        return;

    if (impl->maximized)
        libdecor_frame_unset_maximized(impl->frame);
    else
        libdecor_frame_set_maximized(impl->frame);
}

bool Window::isVisible()
{
    return impl->mapped;
}

Point Window::getPosition() const
{
    return impl->position;
}

void Window::setPosition(Point position)
{
    impl->setPosition(position);
}

void Window::setMouseLocked(bool locked)
{
    impl->setMouseLocked(locked);
}

bool Window::isMouseLocked() const
{
    return impl->mouseLockIntent;
}

// The native unit here is the evdev keycode.
bool Window::isKeyPressed(uint16_t nativeKeyCode) const
{
    auto* connection = waylandDisplay();

    if (connection == nullptr || connection->getInput() == nullptr)
        return false;

    if (!impl->hasKeyboardFocus())
        return false;

    return connection->getInput()->isKeyPressed(nativeKeyCode);
}

bool Window::isShiftPressed() const
{
    return getModifiers().shift;
}

bool Window::isControlPressed() const
{
    return getModifiers().control;
}

bool Window::isAltPressed() const
{
    return getModifiers().alt;
}

bool Window::isCommandPressed() const
{
    return getModifiers().command;
}

ModifierKeys Window::getModifiers() const
{
    auto* connection = waylandDisplay();

    if (connection == nullptr || connection->getInput() == nullptr)
        return {};

    if (!impl->hasKeyboardFocus())
        return {};

    return connection->getInput()->getModifiers();
}

} // namespace eacp::Graphics
