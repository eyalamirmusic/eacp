#pragma once

#include <eacp/Core/Utils/Common.h>

struct wl_display;
struct wl_surface;

namespace eacp::Graphics
{
class View;

inline constexpr float linuxDefaultBackingScale = 1.0f;

void notifyBackingScaleChanged(View& view);

// The Wayland surface behind a view that presents its own pixels (a GPUView).
// Owned by the window backend; the presenter sets the hooks and draws.
struct ViewSurface
{
    // Null while the view is not on a shown window.
    wl_display* display = nullptr;
    wl_surface* surface = nullptr;

    // Buffer size the compositor expects; zero while there is no surface.
    int pixelWidth = 0;
    int pixelHeight = 0;

    // Pixels per point on the surface.
    float scale = linuxDefaultBackingScale;

    std::function<void()> onAvailable = [] {};

    // Destroy the swapchain and VkSurfaceKHR here.
    std::function<void()> onLost = [] {};

    std::function<void()> onResized = [] {};
    std::function<void()> onRepaint = [] {};
    std::function<void()> onFrameDone = [] {};

    // The request rides on the surface's next commit, so call it before
    // presenting; a second call while one is pending does nothing.
    std::function<void()> requestFrameCallback = [] {};

    bool frameCallbackPending = false;
};

// Marks `view` as one that presents its own pixels and returns its record.
ViewSurface& requestViewSurface(View& view);
} // namespace eacp::Graphics
