#pragma once

#include "LinuxWindowNative-Linux.h"

// The X11 toplevel: an InputOutput child of the root carrying the ICCCM and
// EWMH properties a window manager reads, and behaving the same under Xvfb,
// where there is nobody to read them.

namespace eacp::Graphics
{
// Surfaceless, and so headless in everything but name, when no window could be
// made - the same window a lost connection leaves behind.
std::unique_ptr<LinuxWindowNative>
    makeX11WindowNative(const WindowOptions& options, WindowEvents& events);
} // namespace eacp::Graphics
