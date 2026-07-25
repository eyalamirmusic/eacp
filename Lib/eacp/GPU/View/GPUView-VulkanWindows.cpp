#include "GPUViewSurface-Vulkan.h"

#include "GPUView.h"

#include <eacp/Core/Utils/WinInclude.h>

// The Windows half of Vulkan presentation, off-screen for now.
//
// Vulkan's only Win32 surface is built over an HWND, and a GPUView on Windows
// does not have one: it lives in the DirectComposition visual tree, where
// view.getNativeLayer() is an IDCompositionVisual2 (see GPUView-Windows.cpp,
// which hands DComp a composition swapchain as visual content). A child HWND
// would present, but it would also render above the entire composited tree
// regardless of where the view sits in it -- no blending with the 2D layers
// around it, and no clipping by its ancestors. Apps/Mixed/LayeredViews stacks
// GPU and 2D panels over each other and is exactly what that would break.
//
// Presenting without giving that up means going the other way round: render
// off-screen, export the image through VK_KHR_external_memory_win32, and let the
// composition swapchain that is already attached to the visual read it. That is
// a wider seam than SurfaceHost describes -- there is no VkSurfaceKHR anywhere in
// it -- so it is left for when presentation is wired rather than approximated
// here.
//
// Until then this reports no surface, which is the same answer the seam gives on
// a headless driver: GPUView renders off-screen only. View::renderToImage, the
// GPU snapshot suite and every compute path work; nothing reaches the screen.

namespace eacp::GPU::detail
{
SurfaceHost createSurfaceHost(GPUView&, VkInstance)
{
    return {};
}

void destroySurfaceHost(SurfaceHost& host, VkInstance)
{
    host = {};
}

void resizeSurfaceHost(const SurfaceHost&, GPUView&, float) {}

float surfaceBackingScale(GPUView&)
{
    // The figure the D3D12 backend uses, so a view's pixel size is the same
    // whichever backend drew it -- what BackingScaleTests pins.
    return static_cast<float>(GetDpiForSystem()) / 96.f;
}
} // namespace eacp::GPU::detail
