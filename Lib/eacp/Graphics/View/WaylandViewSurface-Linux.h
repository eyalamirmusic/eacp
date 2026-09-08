#pragma once

#include "ViewSurfaceBackend-Linux.h"

// The Wayland half of a presenting view: a wl_subsurface of the window's own
// surface, sized through wp_viewport where the compositor offers one.

namespace eacp::Graphics
{
struct WaylandWindowSurface;

std::unique_ptr<ViewSurfaceBackend>
    makeWaylandViewSurfaceBackend(WaylandWindowSurface& window);
} // namespace eacp::Graphics
