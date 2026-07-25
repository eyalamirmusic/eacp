#pragma once

#include "../Vulkan/VulkanLoader.h"

// Internal seam between the portable Vulkan GPUView (GPUView-Vulkan.cpp) and the
// per-platform piece that owns a presentable surface. Vulkan itself is portable
// but a surface never is: it comes from a CAMetalLayer on Apple, an HWND on
// Win32, a wl_surface or an X11 window on Linux. Everything platform-specific in
// the Vulkan backend is confined to the implementation of these four functions.

namespace eacp::GPU
{
class GPUView;

namespace detail
{
// The platform object the surface is drawn into, plus the surface itself.
struct SurfaceHost
{
    void* handle = nullptr; // CAMetalLayer on Apple
    VkSurfaceKHR surface = VK_NULL_HANDLE;
};

// Creates the platform layer, attaches it into the view's native layer tree, and
// wraps it in a VkSurfaceKHR. Returns an empty host when the platform or the
// driver cannot present, which leaves the view off-screen-only rather than
// broken.
SurfaceHost createSurfaceHost(GPUView& view, VkInstance instance);

void destroySurfaceHost(SurfaceHost& host, VkInstance instance);

// Tracks the view's bounds and backing scale. Called whenever the view resizes
// or moves to a display with a different scale.
void resizeSurfaceHost(const SurfaceHost& host, GPUView& view, float scale);

// Device pixels per logical point for the display the view is on.
float surfaceBackingScale(GPUView& view);
} // namespace detail
} // namespace eacp::GPU
