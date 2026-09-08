#include "WaylandWindow-Linux.h"

#include "WaylandDisplay-Linux.h"
#include "WaylandInput-Linux.h"

#include <algorithm>
#include <cmath>

namespace eacp::Graphics
{
namespace
{
struct WaylandWindowNative final
    : LinuxWindowNative
    , WaylandWindowSurface
{
    WaylandWindowNative(const WindowOptions& options, WindowEvents& events)
        : state(*this, options, events)
    {
        state.unmap = [this] { unmap(); };

        onKeyboardFocus = [this](bool focused) { state.setActive(focused); };
        onConnectionLost = [this] { connectionLost(); };

        createSurface();
    }

    ~WaylandWindowNative() override
    {
        if (contentView != nullptr)
            linuxUnbindWindowFromContentView(*contentView);

        destroyFrame();

        if (auto* connection = waylandDisplay())
        {
            if (auto* seatInput = connection->getInput())
                seatInput->windowDestroyed(*this);

            if (getSurface() != nullptr)
                connection->unregisterSurface(getSurface());
        }

        buffer.destroy();

        if (fractionalScale != nullptr)
            wp_fractional_scale_v1_destroy(fractionalScale);

        if (viewport != nullptr)
            wp_viewport_destroy(viewport);

        if (getSurface() != nullptr)
            wl_surface_destroy(getSurface());
    }

    void createSurface()
    {
        auto* connection = waylandDisplay();

        if (connection == nullptr || connection->getCompositor() == nullptr)
            return;

        setSurface(wl_compositor_create_surface(connection->getCompositor()));

        auto* surface = getSurface();

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

        if (frame != nullptr || getSurface() == nullptr || connection == nullptr)
            return;

        auto* decorations = connection->getDecorations();

        if (decorations == nullptr)
            return;

        frame = libdecor_decorate(decorations, getSurface(), &frameListener(), this);

        if (frame == nullptr)
            return;

        libdecor_frame_set_title(frame, state.title.c_str());
        libdecor_frame_set_app_id(frame, linuxDefaultAppId);

        if (state.minWidth > 0 || state.minHeight > 0)
            libdecor_frame_set_min_content_size(
                frame, std::max(state.minWidth, 1), std::max(state.minHeight, 1));

        // Pinned as well as unset: a compositor may configure a size no client
        // asked for.
        if (!state.resizable)
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

        if (!state.closable)
            libdecor_frame_unset_capabilities(frame, LIBDECOR_ACTION_CLOSE);

        if (!state.miniaturizable)
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

        state.applyConstraints(width, height);

        auto* newState = libdecor_state_new(width, height);
        libdecor_frame_commit(frame, newState, configuration);
        libdecor_state_free(newState);

        auto windowState = LIBDECOR_WINDOW_STATE_NONE;

        if (libdecor_configuration_get_window_state(configuration, &windowState))
        {
            state.maximized = (windowState & LIBDECOR_WINDOW_STATE_MAXIMIZED) != 0;
            state.setActive((windowState & LIBDECOR_WINDOW_STATE_ACTIVE) != 0);
        }

        state.resizeTo({(float) width, (float) height});
        present();

        if (!mapped)
        {
            mapped = true;
            state.notifyHostVisibility(true);
        }

        if (contentView != nullptr)
            linuxWindowSurfaceStateChanged(*contentView);
    }

    void present()
    {
        auto* connection = waylandDisplay();
        auto* surface = getSurface();

        if (surface == nullptr || connection == nullptr)
            return;

        auto width = std::max((int) std::lround(contentSize.x), 1);
        auto height = std::max((int) std::lround(contentSize.y), 1);

        if (viewport != nullptr)
        {
            // One pixel, stretched: a resize costs a viewport request, not a
            // new shm buffer per frame of the drag.
            if (buffer.get() == nullptr)
                buffer.create(connection->getShm(), 1, 1, state.background);

            wp_viewport_set_destination(viewport, width, height);
        }
        else if (buffer.getWidth() != width || buffer.getHeight() != height)
        {
            buffer.create(connection->getShm(), width, height, state.background);
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

        if (state.transparent)
        {
            wl_surface_set_opaque_region(getSurface(), nullptr);
            return;
        }

        auto* region = wl_compositor_create_region(connection->getCompositor());
        wl_region_add(region, 0, 0, width, height);
        wl_surface_set_opaque_region(getSurface(), region);
        wl_region_destroy(region);
    }

    void setVisible(bool shouldBeVisible) override
    {
        if (getSurface() == nullptr)
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
            linuxWindowSurfaceStateChanged(*contentView);

        destroyFrame();
        state.setActive(false);

        wl_surface_attach(getSurface(), nullptr, 0, 0);
        wl_surface_commit(getSurface());
        buffer.destroy();

        if (auto* connection = waylandDisplay())
            connection->flush();

        if (wasMapped)
            state.notifyHostVisibility(false);
    }

    void setTitle(const std::string& newTitle) override
    {
        state.title = newTitle;

        if (frame != nullptr)
            libdecor_frame_set_title(frame, state.title.c_str());
    }

    // The wl_surface. Null wherever no compositor was reached.
    void* getHandle() override { return getSurface(); }

    void minimize() override
    {
        if (frame != nullptr)
            libdecor_frame_set_minimized(frame);
    }

    void toggleMaximize() override
    {
        if (frame == nullptr)
            return;

        if (state.maximized)
            libdecor_frame_unset_maximized(frame);
        else
            libdecor_frame_set_maximized(frame);
    }

    // The compositor went away. Everything made from the connection is
    // dropped, including the view surfaces, whose onLost has to fire while
    // their wl_surface is still a live object; what is left is the window a
    // headless build has.
    void connectionLost()
    {
        auto wasMapped = mapped;
        mapped = false;

        if (contentView != nullptr)
            linuxWindowSurfaceStateChanged(*contentView);

        destroyFrame();
        state.setActive(false);
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

        if (getSurface() != nullptr)
        {
            wl_surface_destroy(getSurface());
            setSurface(nullptr);
        }

        if (wasMapped)
            state.notifyHostVisibility(false);
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
        linuxWindowSurfaceStateChanged(*contentView);
    }

    void setMouseLocked(bool locked) override
    {
        mouseLockIntent = locked;

        if (auto* connection = waylandDisplay())
            if (auto* seatInput = connection->getInput())
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

    // The seat, and only while the keys it is reporting are going to this
    // window.
    WaylandInput* focusedInput() const
    {
        auto* connection = waylandDisplay();

        if (connection == nullptr)
            return nullptr;

        auto* seatInput = connection->getInput();

        if (seatInput == nullptr || seatInput->getKeyboardFocus() != this)
            return nullptr;

        return seatInput;
    }

    LinuxWindowState& getState() override { return state; }

    static WaylandWindowNative& self(void* data)
    {
        return *static_cast<WaylandWindowNative*>(data);
    }

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
            { self(data).state.closeRequested(); };

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

    LinuxWindowState state;

    libdecor_frame* frame = nullptr;
    wp_viewport* viewport = nullptr;
    wp_fractional_scale_v1* fractionalScale = nullptr;
    WaylandShmBuffer buffer;
};
} // namespace

std::unique_ptr<LinuxWindowNative>
    makeWaylandWindowNative(const WindowOptions& options, WindowEvents& events)
{
    return std::make_unique<WaylandWindowNative>(options, events);
}
} // namespace eacp::Graphics
