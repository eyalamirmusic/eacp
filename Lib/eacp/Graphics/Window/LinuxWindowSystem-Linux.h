#pragma once

#include "../Primitives/Primitives.h"

#include <eacp/Core/App/Clipboard-Linux.h>

#include <optional>

// Which window system this copy of eacp talks to, and the process-wide
// questions that have no window to hang off. X11 is not implemented yet: a
// copy that prefers it gets surfaceless windows, exactly as one with no
// compositor to reach does.

namespace eacp::Graphics
{
struct LinuxWindowSurface;

enum class LinuxWindowSystem
{
    None,
    Wayland,
    X11
};

// EACP_WINDOW_SYSTEM names one outright; otherwise a plugin takes X11, because
// every plugin API hands its window out as an X11 id, and a standalone app
// takes the compositor when one answers and X11 when none does. None under
// EACP_HEADLESS. Decided once per copy.
LinuxWindowSystem linuxPreferredWindowSystem();

// The preferred backend's primary output, and nothing when it has no
// connection, no output, or no mode yet.
struct LinuxOutput
{
    Rect frame;
    float scale = 1.f;

    // Millihertz, so 60 Hz is 60000.
    int refreshMilliHz = 0;
};

std::optional<LinuxOutput> linuxPrimaryOutput();

// The seat, as much of it as a View needs. Null and {} until the pointer has
// entered a window of ours.
LinuxWindowSurface* linuxPointerWindow();
Point linuxPointerPosition();

// Re-reads the cursor shape under the pointer and applies it.
void linuxRefreshCursor();

// The clipboard belongs to the preferred backend alone, so a second connection
// opened for a window does not take the selection with it. Called as a
// backend's connection comes up and again on the way down.
void linuxInstallClipboard(LinuxWindowSystem system, Clipboard::Backend backend);
void linuxClearClipboard(LinuxWindowSystem system);
} // namespace eacp::Graphics
