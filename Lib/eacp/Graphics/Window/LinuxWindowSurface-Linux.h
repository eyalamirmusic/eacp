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

// The window `view` is in, through its root; null while it is in none.
LinuxWindowSurface* linuxWindowForView(View& view);

// A view that keeps a native window of its own in step with the tree, which is
// what a NativeChildSurface's container is. Unlike a presenting view's child,
// it is never destroyed on the way: a foreign client is parented into it.
class LinuxNativeChild
{
public:
    virtual ~LinuxNativeChild() = default;

    // The view's bounds, visibility, window or scale may have changed.
    virtual void sync() = 0;

    // The view is leaving its window, which is still alive.
    virtual void release() = 0;
};

void linuxRegisterNativeChild(View& view, LinuxNativeChild& child);
void linuxUnregisterNativeChild(View& view);
} // namespace eacp::Graphics
