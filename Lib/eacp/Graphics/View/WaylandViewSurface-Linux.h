#pragma once

#include "../Primitives/Primitives.h"

// What Window-Linux.cpp needs of View-Linux.cpp.

namespace eacp::Graphics
{
class View;
struct WaylandWindowSurface;

void waylandBindWindowToContentView(View& contentView, WaylandWindowSurface& window);

// Must run before the window's own wl_surface is destroyed.
void waylandUnbindWindowFromContentView(View& contentView);

void waylandWindowSurfaceStateChanged(View& contentView);

// In the window's content points.
Point waylandViewOriginInWindow(const View& view);
} // namespace eacp::Graphics
