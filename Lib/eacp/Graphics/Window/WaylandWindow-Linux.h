#pragma once

#include "LinuxWindowNative-Linux.h"

// The Wayland toplevel: a wl_surface under a libdecor frame.

namespace eacp::Graphics
{
// Surfaceless, and so headless in everything but name, when no compositor
// answered — the same window a lost connection leaves behind.
std::unique_ptr<LinuxWindowNative>
    makeWaylandWindowNative(const WindowOptions& options, WindowEvents& events);
} // namespace eacp::Graphics
