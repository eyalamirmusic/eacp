#pragma once

#include <eacp/Core/Utils/Common.h>

struct wl_display;
struct wl_surface;

namespace eacp::Graphics
{
class View;

// Pixels per point a Linux View reports while nothing has said otherwise.
inline constexpr float linuxDefaultBackingScale = 1.0f;

// Announces a backing-scale change to `view` and every view under it.
//
// Windows never calls View::backingScaleChanged at all, and gets away with it
// because a DPI change there always arrives with a WM_SIZE behind it, so
// anything sized in device pixels is rebuilt by the resize. Wayland's
// wp_fractional_scale and wl_surface.preferred_buffer_scale carry no size with
// them, so on Linux the news has to travel on its own or a glyph atlas
// rasterized at 2x goes on being drawn at 1x.
//
// Called by the window backend when the compositor reports a new scale for the
// surface a view tree is on.
void notifyBackingScaleChanged(View& view);

// The Wayland side of a view that puts its own pixels on screen - a GPUView -
// which is the one kind of view Linux gives a surface of its own.
//
// The window backend (View-Linux.cpp, Window-Linux.cpp) owns everything here:
// it gives a presenting view a wl_subsurface of its window's surface, keeps it
// at the view's bounds, sizes it by the compositor's scale, takes it away when
// the view leaves the window, is hidden, or the window goes, and relays the
// compositor's frame callbacks. The presenter (GPUView-Linux.cpp) owns nothing
// but the hooks it sets and what it draws: a VkSurfaceKHR over `surface`, a
// swapchain at `pixelWidth` x `pixelHeight`, and a present per frame.
//
// This is the whole of what the GPU module knows about Wayland, and it knows
// it as two opaque pointers: eacp-graphics links libwayland-client, eacp-gpu
// does not, and vulkan_wayland.h needs only the forward declarations above.
//
// Everything fires on the main thread, and the record lives as long as the
// View does.
struct ViewSurface
{
    // The connection and the view's own wl_surface. Both null while the view is
    // not in a window that is shown on a compositor - before it joins one, while
    // it or its window is hidden, under EACP_HEADLESS, and when no compositor
    // could be reached at all (no WAYLAND_DISPLAY). Read them only from inside
    // onAvailable and after it, until onLost.
    wl_display* display = nullptr;
    wl_surface* surface = nullptr;

    // The buffer size the compositor expects, in pixels: the view's bounds times
    // `scale`, rounded to whole pixels. Zero while there is no surface. The
    // window backend keeps these current and fires onResized when they change;
    // the swapchain is rebuilt to match at the presenter's next frame.
    int pixelWidth = 0;
    int pixelHeight = 0;

    // Pixels per point on the surface: wp_fractional_scale_v1's preferred scale
    // where the compositor offers it, wl_surface.preferred_buffer_scale or the
    // output's integer scale otherwise. What GPUView::backingScale() reports.
    float scale = linuxDefaultBackingScale;

    // Set by the presenter. Every default is a no-op so the backend calls them
    // unconditionally.
    //
    // onAvailable: display and surface just became non-null. Create the
    // VkSurfaceKHR and the swapchain here (or lazily at the first frame).
    std::function<void()> onAvailable = [] {};

    // onLost: fires BEFORE display and surface are torn down and set to null.
    // The swapchain and the VkSurfaceKHR must be destroyed inside it - a
    // swapchain outliving its wl_surface is a use-after-free inside the driver.
    // Also fires from the View's destructor while the surface is up, and from
    // the Window's destructor for every presenting view under it.
    std::function<void()> onLost = [] {};

    // onResized: pixelWidth, pixelHeight or scale changed while the surface is
    // up. The presenter recreates its swapchain at the next frame; the backend
    // has already called View::resized / notifyBackingScaleChanged as
    // appropriate, so the view's own geometry is current when this fires.
    std::function<void()> onResized = [] {};

    // onRepaint: the view was asked to draw - View::repaint() on Linux ends
    // here, coalesced so any number of repaint() calls in one turn of the loop
    // are one call, made from the loop after the caller returns. GPUView's
    // paint(Context&) never runs on Linux (there is no Context), so this is the
    // on-demand render path: the presenter renders and presents one frame.
    std::function<void()> onRepaint = [] {};

    // onFrameDone: the wl_surface.frame callback the presenter asked for with
    // requestFrameCallback arrived - the compositor has taken the last frame
    // and is ready for another. Continuous mode is paced by this: render,
    // requestFrameCallback, present, wait for onFrameDone, repeat - so a hidden
    // or occluded window, for which the compositor sends no frame callbacks,
    // stops the loop rather than blocking the main thread inside
    // vkAcquireNextImageKHR.
    std::function<void()> onFrameDone = [] {};

    // Asks for one wl_surface.frame callback on `surface`; the request is
    // carried by the surface's next commit, which the swapchain's present
    // makes, so call it BEFORE the present. Implemented by the window backend;
    // a no-op while there is no surface. One outstanding at a time: while
    // frameCallbackPending is true a second call does nothing.
    std::function<void()> requestFrameCallback = [] {};

    // True from requestFrameCallback until onFrameDone, and reset to false by
    // the backend whenever the surface goes away (a callback that will never
    // arrive must not hold the presenter forever).
    bool frameCallbackPending = false;
};

// Marks `view` as one that presents its own pixels and returns its record. The
// first call is what makes the window backend give the view a subsurface when
// it is in a shown window; a view that never calls this stays a node in the
// tree with no surface of its own, which is what every non-GPU view is.
//
// Safe to call before the view is in a window: the record exists from
// construction, the surface arrives with onAvailable.
ViewSurface& requestViewSurface(View& view);
} // namespace eacp::Graphics
