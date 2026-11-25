#pragma once

#include "ViewSurfaceBackend-Linux.h"
#include "../Window/LinuxWindowSurface-Linux.h"

#include <string>

// The web half of a presenting view. A window is one <canvas>, so the whole
// canvas goes to one presenting view at a time, as Android's one surface does.

namespace eacp::Graphics
{
struct WebWindowSurface : LinuxWindowSurface
{
    int windowId = 0;

    // "#eacp-canvas-1"; empty while there is no canvas. Never reassigned once
    // set, since nativeSurface.surface points into it.
    std::string canvasSelector;

    // The canvas's width and height attributes: its CSS size times the scale.
    int pixelWidth = 0;
    int pixelHeight = 0;
};

std::unique_ptr<ViewSurfaceBackend>
    makeWebViewSurfaceBackend(WebWindowSurface& window);
} // namespace eacp::Graphics
