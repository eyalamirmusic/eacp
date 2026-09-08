#pragma once

#include "View-Linux.h"

#include <memory>

// The window-system half of a presenting view's surface. View-Linux.cpp keeps
// the records and decides when a view should have one; these two say what a
// native child actually is.

namespace eacp::Graphics
{
class View;

// One per presenting view, made by the window's backend. Destroying it takes
// the native child away, after the record's onLost has fired.
class ViewSurfaceNative
{
public:
    virtual ~ViewSurfaceNative() = default;

    // Moves and resizes the child to the view's bounds. True when the pixel
    // size or the scale changed, which is what earns the record an onResized.
    virtual bool applyGeometry() = 0;

    // Asks for the next frame; the record's onFrameDone answers it.
    virtual void requestFrame() = 0;
};

// One per window.
class ViewSurfaceBackend
{
public:
    virtual ~ViewSurfaceBackend() = default;

    // A native child at the view's bounds, with `record`'s handle, pixel size
    // and scale filled in. Null when the window system could not make one.
    virtual std::unique_ptr<ViewSurfaceNative>
        createSurface(View& view, ViewSurface& record) = 0;
};
} // namespace eacp::Graphics
