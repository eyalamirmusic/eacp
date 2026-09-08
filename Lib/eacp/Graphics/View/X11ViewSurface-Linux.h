#pragma once

#include "ViewSurfaceBackend-Linux.h"

// The X11 half of a presenting view: an InputOutput child of the toplevel,
// moved and sized with xcb_configure_window, and answered by a timer because
// X11 has no wl_surface.frame (plan.md D7).

namespace eacp::Graphics
{
struct X11WindowSurface;

std::unique_ptr<ViewSurfaceBackend>
    makeX11ViewSurfaceBackend(X11WindowSurface& window);
} // namespace eacp::Graphics
