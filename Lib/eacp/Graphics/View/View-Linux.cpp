#include "View-Linux.h"

#include "View.h"
#include "../Image/Image.h"

#include <eacp/Core/Threads/Async.h>

// The Linux View, with no window system under it.
//
// Everything a View is made of that matters to app code — the tree, the
// bounds, the hit-testing, the mouse routing, the visibility bookkeeping — is
// already portable and lives in View.cpp. What a backend adds is a native
// surface per view, and Linux does not have one yet: the compositor half is
// plan stage 4 and the only thing that will ever want a surface of its own is
// a GPUView, which parents into this rather than the other way round. So the
// native side here is bounds and a focus flag, and the interesting decision is
// which of the two Windows shapes to copy.
//
// It copies the one where a View is NOT a native window: one surface per
// Window, none per View, all input routed by the portable hit-tester. That is
// what makes a Wayland subsurface an addition to this file later rather than a
// rewrite of it.

namespace eacp::Graphics
{

struct View::Native
{
    explicit Native(View* owner)
        : ownerView(owner)
    {
    }

    Rect getBounds() const { return bounds; }

    void setBounds(const Rect& newBounds)
    {
        bounds = newBounds;
        ownerView->resized();
    }

    void focus() { focused = true; }
    bool hasFocus() const { return focused; }

    View* ownerView;
    Rect bounds;
    bool focused = false;
};

View::View()
    : impl(this)
{
}

View::~View()
{
    for (auto* layer: getLayers())
        layer->detachFromLayer();

    removeFromParent();
}

// There is no native object behind a Linux View yet, so both of these hand
// back the View::Native itself: a stable, non-null identity for this view that
// a GPUView can hold on to now and that grows a wl_subsurface inside it at
// stage 4, without the handle a caller stored changing underneath it.
void* View::getHandle()
{
    return impl.get();
}

void* View::getNativeLayer()
{
    return impl.get();
}

// Nothing composites a Linux View, so there is no dirty region to mark and no
// frame to schedule. A view that calls repaint() every frame costs nothing
// here rather than being wrong.
void View::repaint() {}

void View::setOpacity(float opacityToUse)
{
    opacity = opacityToUse;
}

void View::setVisible(bool shouldBeVisible)
{
    if (visible == shouldBeVisible)
        return;

    visible = shouldBeVisible;
    notifyVisibilityChanged(shouldBeVisible);
}

Rect View::getBounds() const
{
    return impl->getBounds();
}

void View::setBounds(const Rect& bounds)
{
    impl->setBounds(bounds);
}

// No pointer without a compositor. Reported as the view's origin rather than
// as something invented, so isHovering() is consistently false for a view that
// is not at {0, 0}.
Point View::getMousePosition() const
{
    return {};
}

// Remembered and never shown, exactly as on Windows and iOS: the shape is a
// property of the view either way, and portable code sets it unconditionally.
// wl_pointer.set_cursor is what acts on it, and that needs a pointer to have
// entered a surface (stage 4).
void View::setMouseCursor(MouseCursor cursor)
{
    currentCursor = cursor;
}

void View::focus()
{
    impl->focus();
}

bool View::hasFocus() const
{
    return impl->hasFocus();
}

void notifyBackingScaleChanged(View& view)
{
    view.backingScaleChanged();

    for (auto* child: view.getSubviews())
        notifyBackingScaleChanged(*child);
}

// Straight to the view's own native content, with no compositing pass around
// it. On macOS and Windows this walks the tree drawing paint(), the attached
// layers and each child through a 2D context; there is no 2D context on Linux,
// so what a snapshot can honestly contain is what the view renders itself —
// which is the GPU read-back path, and the one the GPU tests ride on.
//
// A plain View therefore snapshots as an invalid Image (View::renderNativeContent
// returns one), the same answer it gives for a zero-sized view.
Image View::renderToImage(float scale)
{
    auto resolvedScale = scale > 0.0f ? scale : linuxDefaultBackingScale;

    return renderNativeContent(resolvedScale);
}

Threads::Async<Image> View::renderToImageAsync(float scale)
{
    auto promise = Threads::AsyncPromise<Image> {};
    auto result = promise.get();

    // Nothing here is asynchronous: the async form exists for embedded web
    // content, which needs a native web runtime Linux has none of.
    promise.resolve(renderToImage(scale));

    return result;
}

void View::viewAdded(View&) {}
void View::viewRemoved(View&) {}

} // namespace eacp::Graphics
