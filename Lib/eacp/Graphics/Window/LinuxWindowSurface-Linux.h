#pragma once

#include "../Primitives/Primitives.h"
#include "../View/ViewSurfaceBackend-Linux.h"

// What the input and view-surface code need of a Window, whatever window
// system is under it. Every backend's Window::Native derives from this.

namespace eacp::Graphics
{
class View;

struct LinuxWindowSurface
{
    virtual ~LinuxWindowSurface() = default;

    // Kind::None while the window is headless or the connection failed.
    NativeSurfaceHandle nativeSurface;

    // Set by the native as it is built; every presenting view of this window
    // asks it for a child surface.
    std::unique_ptr<ViewSurfaceBackend> viewSurfaces;

    View* contentView = nullptr;

    // In points, and so are pointer positions on the surface.
    Point contentSize;

    float scale = linuxDefaultBackingScale;
    bool mapped = false;

    // Intent only; a real pointer lock also needs keyboard focus.
    bool mouseLockIntent = false;

    std::function<void(bool)> onKeyboardFocus = [](bool) {};

    // The server went away mid-session: drop the surface and everything made
    // from it, and report the window hidden.
    Callback onConnectionLost = [] {};
};

// What Window-Linux.cpp needs of View-Linux.cpp.

void linuxBindWindowToContentView(View& contentView, LinuxWindowSurface& window);

// Must run before the window's own native surface is destroyed.
void linuxUnbindWindowFromContentView(View& contentView);

void linuxWindowSurfaceStateChanged(View& contentView);

// In the window's content points.
Point linuxViewOriginInWindow(const View& view);
} // namespace eacp::Graphics
