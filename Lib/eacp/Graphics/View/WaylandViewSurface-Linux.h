#pragma once

#include "../Primitives/Primitives.h"

// What Window-Linux.cpp needs of View-Linux.cpp.
//
// A presenting view's wl_subsurface is made, moved, resized and destroyed from
// View-Linux.cpp, because that is the only file that can see View::Native and
// therefore the only one that can hold the ViewSurface record the GPU module
// reads. The window, meanwhile, is the thing that knows when there is a
// surface to hang subsurfaces off at all - so the two meet here, at three
// calls and one lookup.
//
// The lookup is the Windows shape: View-Linux.cpp keeps a content-view to
// window map, exactly as CompositionHostWindow-Windows.cpp keeps a content-view
// to HWND one, so any view in the tree finds its window's surface by walking up
// to the root rather than by carrying a back-pointer that would have to be
// maintained on every reparent.

namespace eacp::Graphics
{
class View;
struct WaylandWindowSurface;

// The window adopted `contentView`. From here every presenting view under it
// gets a subsurface as soon as the window is mapped.
void waylandBindWindowToContentView(View& contentView, WaylandWindowSurface& window);

// The window is going, or has dropped this content view. Fires ViewSurface's
// onLost for every presenting view under it and destroys the subsurfaces,
// which has to happen BEFORE the window's own wl_surface does: a swapchain
// outliving the surface it was made from is a use-after-free inside the driver.
void waylandUnbindWindowFromContentView(View& contentView);

// The window's `mapped` or `scale` changed. Reconciles every view surface
// under it against the new state - creating the ones that should now exist,
// tearing down the ones that should not, and reporting a new pixel size to the
// ones that stay.
void waylandWindowSurfaceStateChanged(View& contentView);

// The view's top-left in its window's content points: its bounds' origin plus
// every ancestor's, up to but not including the content view, whose own bounds
// are the window's content rect. Used to lift a pointer position off a
// subsurface back into window coordinates.
Point waylandViewOriginInWindow(const View& view);
} // namespace eacp::Graphics
