#include "WaylandViewSurface-Linux.h"

#include "View.h"
#include "../Window/WaylandDisplay-Linux.h"

#include <algorithm>
#include <cmath>

// File-scope names carry a wayland/Wayland prefix: this is one unity TU under
// EACP_CI_BUILD.

namespace eacp::Graphics
{
namespace
{
int waylandRoundToPixels(float points, float scale)
{
    return std::max((int) std::lround(points * scale), 1);
}

class WaylandViewSurfaceNative : public ViewSurfaceNative
{
public:
    WaylandViewSurfaceNative(View& viewToUse,
                             WaylandWindowSurface& windowToUse,
                             ViewSurface& recordToUse)
        : view(viewToUse)
        , window(windowToUse)
        , record(recordToUse)
    {
    }

    ~WaylandViewSurfaceNative() override
    {
        if (surface == nullptr)
            return;

        if (frameCallback != nullptr)
            wl_callback_destroy(frameCallback);

        if (auto* connection = waylandDisplay())
            connection->unregisterSurface(surface);

        if (viewport != nullptr)
            wp_viewport_destroy(viewport);

        if (subsurface != nullptr)
            wl_subsurface_destroy(subsurface);

        wl_surface_destroy(surface);
    }

    bool create()
    {
        auto* connection = waylandDisplay();

        if (connection == nullptr || connection->getCompositor() == nullptr
            || connection->getSubcompositor() == nullptr)
            return false;

        surface = wl_compositor_create_surface(connection->getCompositor());

        if (surface == nullptr)
            return false;

        subsurface = wl_subcompositor_get_subsurface(
            connection->getSubcompositor(), surface, window.getSurface());

        // Desync, so the presenter's commits do not wait for the window's.
        wl_subsurface_set_desync(subsurface);

        if (auto* viewporter = connection->getViewporter())
            viewport = wp_viewporter_get_viewport(viewporter, surface);

        connection->registerSurface({surface, &window, &view});

        applyGeometry();

        record.handle = {NativeSurfaceHandle::Kind::Wayland,
                         connection->getDisplay(),
                         surface,
                         0};

        return true;
    }

    // wp_viewporter is the only way to express a fractional scale; without it
    // the buffer scale is a whole number.
    bool applyGeometry() override
    {
        auto bounds = view.getBounds();
        auto origin = linuxViewOriginInWindow(view);

        wl_subsurface_set_position(subsurface,
                                   (int32_t) std::lround(origin.x),
                                   (int32_t) std::lround(origin.y));

        auto scale = window.scale;
        auto pixelWidth = 0;
        auto pixelHeight = 0;

        if (viewport != nullptr)
        {
            pixelWidth = waylandRoundToPixels(bounds.w, scale);
            pixelHeight = waylandRoundToPixels(bounds.h, scale);

            wp_viewport_set_destination(
                viewport,
                std::max((int32_t) std::lround(bounds.w), 1),
                std::max((int32_t) std::lround(bounds.h), 1));
        }
        else
        {
            auto wholeScale = std::max((int) std::lround(scale), 1);

            wl_surface_set_buffer_scale(surface, wholeScale);

            pixelWidth = std::max((int) std::lround(bounds.w), 1) * wholeScale;
            pixelHeight = std::max((int) std::lround(bounds.h), 1) * wholeScale;
            scale = (float) wholeScale;
        }

        auto changed = pixelWidth != record.pixelWidth
                       || pixelHeight != record.pixelHeight || scale != record.scale;

        record.pixelWidth = pixelWidth;
        record.pixelHeight = pixelHeight;
        record.scale = scale;

        // A subsurface's position is applied by its parent's commit, not its
        // own.
        wl_surface_commit(window.getSurface());

        return changed;
    }

    void requestFrame() override
    {
        frameCallback = wl_surface_frame(surface);
        wl_callback_add_listener(frameCallback, &frameListener(), this);

        record.frameCallbackPending = true;

        // The request rides on a commit, and the caller may have nothing to
        // present: an empty commit re-sends the buffer that is already there,
        // which is what keeps a paced loop ticking. Before the first buffer
        // there is nothing to re-send and no compositor would answer, so that
        // one waits for the commit that maps the surface.
        if (!presented)
            return;

        wl_surface_commit(surface);

        if (auto* connection = waylandDisplay())
            connection->flush();
    }

private:
    void frameDone(wl_callback* callback)
    {
        if (frameCallback == callback)
        {
            wl_callback_destroy(frameCallback);
            frameCallback = nullptr;
        }

        record.frameCallbackPending = false;
        presented = true;

        record.onFrameDone();
    }

    static const wl_callback_listener& frameListener()
    {
        static const wl_callback_listener table {
            .done = [](void* data, wl_callback* callback, uint32_t)
            { static_cast<WaylandViewSurfaceNative*>(data)->frameDone(callback); },
        };

        return table;
    }

    View& view;
    WaylandWindowSurface& window;
    ViewSurface& record;

    wl_surface* surface = nullptr;
    wl_subsurface* subsurface = nullptr;
    wp_viewport* viewport = nullptr;
    wl_callback* frameCallback = nullptr;

    // A frame callback only ever arrives after a commit that carried a buffer,
    // so this is how the surface says it has content to re-commit.
    bool presented = false;
};

class WaylandViewSurfaceBackend : public ViewSurfaceBackend
{
public:
    explicit WaylandViewSurfaceBackend(WaylandWindowSurface& windowToUse)
        : window(windowToUse)
    {
    }

    std::unique_ptr<ViewSurfaceNative> createSurface(View& view,
                                                     ViewSurface& record) override
    {
        auto native =
            std::make_unique<WaylandViewSurfaceNative>(view, window, record);

        if (!native->create())
            return {};

        return native;
    }

private:
    WaylandWindowSurface& window;
};
} // namespace

std::unique_ptr<ViewSurfaceBackend>
    makeWaylandViewSurfaceBackend(WaylandWindowSurface& window)
{
    return std::make_unique<WaylandViewSurfaceBackend>(window);
}
} // namespace eacp::Graphics
