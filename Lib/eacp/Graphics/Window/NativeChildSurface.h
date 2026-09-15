#pragma once

#include "../View/View.h"

namespace eacp::Graphics
{

// A rectangle in one of our layouts that somebody else's native view is
// parented into — the inverse of EmbeddedView.
//
// EmbeddedView puts a surface of ours inside a window another framework owns.
// This is the other direction: the layout is ours and the content is not. A
// plugin host is where that happens. A hosted plugin's editor is an NSView or
// an HWND the plugin built, owns and draws into itself, and all a host can do
// is give it a well-placed rectangle of its own window to live in and tell it
// how big that rectangle is.
//
// It is a View, so it is placed with setBounds, added with addSubview and
// follows its parent the way every other view does, and getLocalBounds is the
// size the foreign content has been given. What it adds is a native handle for
// the foreign toolkit to parent into, and the promise that whatever is behind
// that handle covers this view's bounds and goes on covering them.
//
// The foreign content's lifetime is never ours. Destroying the surface takes
// down the container and nothing else: content still attached to it must be
// detached first — a VST3 plugin is sent removed() — and after that the
// plugin's own view is the plugin's business.
class NativeChildSurface : public View
{
public:
    NativeChildSurface();
    ~NativeChildSurface() override;

    // The handle a foreign toolkit parents its own view into: an NSView* on
    // macOS, a child HWND on Windows — which are the two things a plugin format
    // names as its platform view type, so this is what goes straight into
    // IPlugView::attached() beside kPlatformTypeNSView / kPlatformTypeHWND.
    //
    // Deliberately not View::getHandle(), which is the handle eacp draws
    // through and is an IDCompositionVisual2 on Windows — nothing foreign can
    // be parented into one of those. The two answers differ per platform on
    // purpose and only this one is a parent.
    //
    // Null until the surface has a native window to sit in, which on Windows
    // means until this view is in a Window or an EmbeddedView: a child window
    // needs a parent window at creation, unlike an NSView. Ask for it once the
    // view is parented — an Editor's onAttached(), which is exactly when a
    // plugin format wants it — and not from a constructor.
    void* getNativeParentHandle();

    // Places the native child over this view's bounds again.
    //
    // Called for you whenever this view resizes, which covers a layout that
    // moves it by giving it new bounds. The case it does not cover is Windows,
    // where the child is a window positioned in its top-level window's
    // coordinates rather than a subview of its parent: an *ancestor* moving
    // while this view keeps the same bounds moves the surface without resizing
    // it, and there is no notification for that. A container that shifts its
    // children without changing their size calls this afterwards.
    void refreshPlacement();

    void resized() override;
    void visibilityChanged(bool nowVisible) override;

private:
    struct Native;
    Pimpl<Native> impl;
};

} // namespace eacp::Graphics
