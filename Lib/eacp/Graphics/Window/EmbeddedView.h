#pragma once

/** @file EmbeddedView: a surface of ours inside a native view or window a host
    owns (plugin editors). */

#include "../View/View.h"

namespace eacp::Graphics
{

// The surface's initial size in points, used until the host resizes it.
struct EmbeddedViewOptions
{
    int width = 640;
    int height = 400;
};

// A surface of ours inside a window somebody else owns.
//
// The host hands over a native handle — an NSView* on macOS, a UIView* on iOS,
// an HWND on Windows, an X11 window id on Linux, where the id is a number and
// travels in the pointer parameter every plugin API passes one in — and
// everything inside the surface built against it is ours: a content View, its
// subviews, its GPU content, its input.
//
// By default the surface fills the host and follows its resizes. A host with
// its own layout places it with setBounds instead.
class EmbeddedView
{
public:
    EmbeddedView(void* hostParentHandle,
                 const EmbeddedViewOptions& optionsToUse = {});
    ~EmbeddedView();

    void setContentView(View& view);

    // Places the surface in the host, in points, y-down from the host's
    // top-left on every platform (converted and rounded outwards to pixels as
    // needed). The first call turns off automatic fill for good; from then on
    // the host must call it again whenever its layout changes.
    void setBounds(const Rect& newBounds);

    // Where the surface is, in that same space: what was last set, or the
    // options' size at the origin.
    constexpr Rect getBounds() const { return bounds; }

    // Resizes the surface about its top-left, leaving the automatic sizing
    // alone — the call a host that handed over its whole window makes when
    // that window resizes.
    void setSize(int width, int height);

    // Shows or hides the surface without destroying it: the content View, its
    // GPU resources and any WebView state stay alive and it comes back exactly
    // as it was, which destroying the EmbeddedView and building another does
    // not give you.
    //
    // This is the surface's own flag, and not the same as View::setVisible on
    // the content. That hides what the surface draws and leaves the surface
    // itself in the host's window — where on Windows it is still a live child
    // window, taking the mouse over its rectangle from whatever the host has
    // underneath.
    void setVisible(bool shouldBeVisible);
    constexpr bool isVisible() const { return visible; }

    // How many physical pixels the surface should put in a point.
    //
    // A host that scales its own window decides this, and the surface cannot
    // read the answer off the window: when the host is doing the scaling, the
    // figure it works in and the one the display reports are different
    // numbers. Everything the surface measures follows from it — where it is
    // placed in pixels, the size in points its content View is given, where a
    // mouse event lands — so it is one answer for the whole surface rather
    // than a correction applied to its frame.
    //
    // 0, the default, follows the platform: on Windows, the window's own DPI;
    // on Linux one pixel per point, because X11 has no per-window scale to
    // read and the host is the only thing that knows better. Ignored on macOS
    // and iOS, where points are the platform's own space and AppKit or UIKit
    // does this conversion itself, from the screen the surface is on.
    void setPixelsPerPoint(float pixelsPerPoint);

    // The surface's own native handle, in the same currency the host's came
    // in: an NSView*, a UIView*, an HWND, and on Linux the child window's id
    // widened into the pointer.
    void* getHandle();

private:
    Rect bounds;
    bool visible = true;

    struct Native;
    Pimpl<Native> impl;
};

} // namespace eacp::Graphics
