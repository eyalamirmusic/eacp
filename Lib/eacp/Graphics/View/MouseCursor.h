#pragma once

#include "../Primitives/Primitives.h"

#include <memory>

namespace eacp::Graphics
{

class Image;

// The shape the pointer takes over a view: one of the shapes every platform
// names the same way, or a picture of the app's own.
//
// The named shapes are the common case, and spelled the way they always were —
// MouseCursor::IBeam converts to a cursor wherever one is taken. The image form
// is what a tool needs whose pointer *is* the tool: a pencil, a velocity brush.
//
// A handle, cheap to copy and to compare: an image cursor shares one record
// between its copies, and two cursors made from the same pixels, hotspot and
// scale compare equal without reading the pixels. The record holds its own copy
// of the image, and Image is copy-on-write, so a later write to the caller's
// image never reaches a cursor already made from it.
class MouseCursor
{
public:
    enum class Shape
    {
        // The arrow. What a view has until it says otherwise.
        Default,

        // Over text that can be selected.
        IBeam,

        // Over something clickable that is not a control — a link.
        PointingHand,

        // Over a vertical splitter, so a draggable divider reads as draggable
        // before anyone tries dragging it.
        ResizeLeftRight,

        // Over a horizontal splitter.
        ResizeUpDown,

        Crosshair
    };

    using enum Shape;

    MouseCursor(Shape shapeToUse = Shape::Default);

    // `hotspot` is the point of the image that sits under the pointer, in the
    // image's pixels from its top-left corner. `scale` is how many image pixels
    // make one point: a 32-pixel image at scale 4 shows 8 points across, drawn
    // from all 32 pixels on a display dense enough to use them. An invalid image
    // is the arrow.
    MouseCursor(const Image& image, Point hotspot, float scale = 1.f);

    // The named shape, or Default for an image cursor — which is also what a
    // platform that cannot show an image shows instead.
    Shape getShape() const;

    bool isImage() const;

    // Image cursors only; an invalid image, a zero hotspot and 1 otherwise.
    Image getImage() const;
    Point getHotspot() const;
    float getScale() const;

    bool operator==(const MouseCursor& other) const;

    // Where a backend parks the native cursor it built from the image, shared by
    // every copy and released with the last of them. Null for a named shape.
    std::shared_ptr<void>* getNativeCache() const;

private:
    struct Custom;

    Shape shape = Shape::Default;
    std::shared_ptr<Custom> custom;
};

} // namespace eacp::Graphics
