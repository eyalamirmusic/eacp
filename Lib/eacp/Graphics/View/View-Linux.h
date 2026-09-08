#pragma once

#include <eacp/Core/Utils/Common.h>

namespace eacp::Graphics
{
class View;

inline constexpr float linuxDefaultBackingScale = 1.0f;

void notifyBackingScaleChanged(View& view);

// The window-system objects a presenter needs to make a rendering surface
// from. Opaque on purpose: the GPU module reads it and links no window system.
struct NativeSurfaceHandle
{
    enum class Kind
    {
        None,
        Wayland,
        X11
    };

    bool isValid() const { return kind != Kind::None; }

    Kind kind = Kind::None;

    // wl_display* or xcb_connection_t*.
    void* connection = nullptr;

    // wl_surface*, and null on X11.
    void* surface = nullptr;

    // xcb_window_t, and zero on Wayland.
    uint32_t window = 0;
};

// The native surface behind a view that presents its own pixels (a GPUView).
// Owned by the window backend; the presenter sets the hooks and draws.
struct ViewSurface
{
    // Kind::None while the view is not on a shown window.
    NativeSurfaceHandle handle;

    // Buffer size the window system expects; zero while there is no surface.
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

    // Asks the window system for the next frame. Once the surface has content
    // it commits for itself, so a tick that presents nothing still earns the
    // callback that paces the one after it; before the first buffer there is
    // nothing to commit and the request rides on the commit that maps the
    // surface. A second call while one is pending does nothing.
    std::function<void()> requestFrameCallback = [] {};

    bool frameCallbackPending = false;
};

// Marks `view` as one that presents its own pixels and returns its record.
ViewSurface& requestViewSurface(View& view);
} // namespace eacp::Graphics
