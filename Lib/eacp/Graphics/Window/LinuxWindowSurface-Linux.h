#pragma once

#include "../Graphics/Keyboard.h"
#include "../Primitives/Primitives.h"
#include "../View/MouseCursor.h"
#include "../View/ViewSurfaceBackend-Linux.h"

// What the input and view-surface code need of a Window, whatever window
// system is under it. Every backend's Window::Native derives from this.

namespace eacp::Graphics
{
class View;
struct MouseEvent;

struct LinuxWindowSurface
{
    virtual ~LinuxWindowSurface() = default;

    // Where the content view goes: everything below the menu bar.
    Rect contentBounds() const;

    // Sets the content view's bounds, and the menu bar's.
    void layoutViews();

    // What the backends hand their pointer and key events to, in the window's
    // points. A pointer event goes to the menu bar or to the content, by where
    // it is, in that view's own coordinates: a press keeps its view until the
    // release, and the view the pointer leaves is told so. While a menu is
    // open, a press outside the bar's view closes it and goes nowhere else.
    // A key goes through keyFilter first.
    void dispatchMouseEvent(const MouseEvent& event);
    void dispatchKeyEvent(const KeyEvent& event);

    // The shape the view under `position` asks for.
    MouseCursor::Shape cursorShapeAt(Point position);

    // Drops whatever routing state names `view`, which is leaving the window.
    void forgetRoot(const View* view);

    // Kind::None while the window is headless or the connection failed.
    NativeSurfaceHandle nativeSurface;

    // Set by the native as it is built; every presenting view of this window
    // asks it for a child surface.
    std::unique_ptr<ViewSurfaceBackend> viewSurfaces;

    View* contentView = nullptr;

    // The whole surface, menu bar included, in points; so are pointer
    // positions on it. What a Window reports as its size leaves contentTop out.
    Point contentSize;

    // The menu bar (Menu/Menu-Linux.cpp): a second root view, bound to this
    // window beside the content and drawn over its top. Null when there is
    // none. Its bounds are where it takes the pointer, which while a menu is
    // open is more than the bar.
    View* menuBarView = nullptr;

    // How far down the content starts: the menu bar's height, or zero.
    float contentTop = 0.f;

    // While true, every pointer event outside the menu bar's view is
    // swallowed, and a press there runs menuBarDismiss.
    bool menuBarModal = false;
    Callback menuBarDismiss = [] {};

    // Places the menu bar's view; run by layoutViews.
    Callback layoutMenuBar = [] {};

    // Sees each key before the content view does, and keeps it by returning
    // true. Copied before it is called, since what it runs may replace it.
    std::function<bool(const KeyEvent&)> keyFilter;

    float scale = linuxDefaultBackingScale;
    bool mapped = false;

    // Intent only; a real pointer lock also needs keyboard focus.
    bool mouseLockIntent = false;

    std::function<void(bool)> onKeyboardFocus = [](bool) {};

    // The server went away mid-session: drop the surface and everything made
    // from it, and report the window hidden.
    Callback onConnectionLost = [] {};

private:
    View* rootAt(Point position) const;

    // The root the pointer was last over, and the one a held button went down
    // in.
    View* hoverRoot = nullptr;
    View* pressRoot = nullptr;
    bool pressSwallowed = false;
};

// What Window-Linux.cpp needs of View-Linux.cpp.

// Binds a root view to the window: the content view, or the menu bar.
void linuxBindWindowToContentView(View& contentView, LinuxWindowSurface& window);

// Must run before the window's own native surface is destroyed.
void linuxUnbindWindowFromContentView(View& contentView);

// Every root bound to the window, for a native going away.
void linuxUnbindWindow(LinuxWindowSurface& window);

// Syncs every root bound to the same window as `contentView`.
void linuxWindowSurfaceStateChanged(View& contentView);

// In the window's content points, the root's own place in the window
// included.
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

// A native child just made, or just moved into the window, under `view` starts
// above its siblings: the menu bar's surfaces go back above it, so an open menu
// still covers it.
void linuxKeepMenuBarAbove(View& view, const NativeSurfaceHandle& created);
} // namespace eacp::Graphics
