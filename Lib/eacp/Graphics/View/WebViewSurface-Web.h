#pragma once

#include "ViewSurfaceBackend-Linux.h"
#include "../Window/LinuxWindowSurface-Linux.h"

// The web half of a presenting view. A page has one canvas, so the window's
// whole canvas goes to one presenting view at a time, as Android's one surface
// does: its NativeSurfaceHandle is Kind::Canvas with the canvas's selector, its
// pixel size the canvas's CSS size times devicePixelRatio, and the drawing
// buffer is already that size when onAvailable or onResized fires.
// onFrameDone answers requestFrameCallback on the next requestAnimationFrame.

namespace eacp::Graphics
{
struct WebWindowSurface : LinuxWindowSurface
{
    // The canvas's drawing buffer; zero while there is no canvas.
    int pixelWidth = 0;
    int pixelHeight = 0;
};

std::unique_ptr<ViewSurfaceBackend>
    makeWebViewSurfaceBackend(WebWindowSurface& window);
} // namespace eacp::Graphics
