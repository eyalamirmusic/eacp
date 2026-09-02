#pragma once

#include "../View/View.h"

namespace eacp::Graphics
{

struct EmbeddedViewOptions
{
    int width = 640;
    int height = 400;
};

// A surface of ours inside a window somebody else owns.
//
// The host hands over a native handle — an NSView* on macOS, an HWND on
// Windows — and everything inside the surface built against it is ours: a
// content View, its subviews, its GPU content, its input.
//
// Two kinds of host embed a surface, and they want opposite things from it.
// One hands over its whole window and has no layout of its own; that is what
// this defaults to, and it needs saying nothing — the surface fills the host
// and goes on filling it as the host resizes. The other has a layout: controls
// of its own around the surface, or several surfaces in one window. That host
// knows where the surface belongs and nothing else does, which is what
// setBounds is for.
class EmbeddedView
{
public:
    EmbeddedView(void* hostParentHandle,
                 const EmbeddedViewOptions& optionsToUse = {});
    ~EmbeddedView();

    void setContentView(View& view);

    // Places the surface inside its host: `newBounds` in points, y-down from
    // the host's top-left, which is the space the rest of eacp measures in
    // (see Primitives.h) whatever the host platform's own convention is. A
    // superview that is not flipped measures its subviews up from its bottom
    // edge and this converts; a child window is placed in physical pixels and
    // this scales, rounding outwards so no seam of the host's window is left
    // showing along an edge.
    //
    // The first call takes the surface off the automatic sizing above, for
    // good. A host with a layout is the authority on where the surface goes,
    // and the two cannot both be writing the frame: the platform's own
    // resizing would win the next time the host window changed size, and it
    // would win during a live resize, where it runs and the host's layout has
    // not caught up. So from here the surface stays where it is put — and
    // placing it again whenever the host's layout moves is the host's job.
    void setBounds(const Rect& newBounds);

    // Where the surface is, in that same space: what was last set, or the
    // options' size at the origin.
    Rect getBounds() const { return bounds; }

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
    bool isVisible() const { return visible; }

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
    // 0, the default, follows the platform: on Windows, the window's own DPI.
    // Ignored on macOS, where points are the platform's own space and AppKit
    // does this conversion itself.
    void setPixelsPerPoint(float pixelsPerPoint);

    void* getHandle();

private:
    Rect bounds;
    bool visible = true;

    struct Native;
    Pimpl<Native> impl;
};

} // namespace eacp::Graphics
