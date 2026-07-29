#pragma once

#include "CoverageAtlas.h"

namespace eacp::UI
{
class Component;

// A vector shape a component draws: the path, the coverage a kernel rasterized
// for it, and the slot of the shared atlas that coverage lives in.
//
// It exists as a member of the widget rather than as an argument to a paint
// call, and that is the whole design. Rasterizing is a compute dispatch, and a
// compute pass can only be recorded *before* the render pass that samples what
// it wrote -- so it cannot happen inside paint(). Setting the path is therefore
// what schedules the work: call it from resized(), or when the value the shape
// depends on changes, and the host runs everything dirty at the top of the very
// next frame, before the pass that draws it. No latency, no stale frame, and
// the cost model is visible in the API - building a shape is a thing you do
// when the geometry changes, and drawing one is a quad.
//
//   struct Knob final : Component
//   {
//       Knob() : indicator(*this) {}
//
//       void resized() override      { rebuild(); }
//       void setValue(float v)       { value = v; rebuild(); repaint(); }
//       void paint(Graphics& g) override { g.fillPath(indicator); }
//
//       void rebuild()
//       {
//           auto path = GPUWidgets::Path {};
//           ...
//           indicator.setPath(path);
//       }
//
//       PathShape indicator;
//   };
//
// The path is authored in the owning component's own points. The device scale
// is the host's business and applied for you, so the same shape is crisp on a
// Retina display and on a conventional one with nothing in the widget saying so.
class PathShape
{
public:
    // Registers with the component that draws it, which is how the host finds
    // it to rasterize. The component has to outlive the shape, which holding it
    // as a member gets you.
    explicit PathShape(Component& ownerToUse);
    ~PathShape();

    PathShape(const PathShape&) = delete;
    PathShape& operator=(const PathShape&) = delete;

    // The geometry, in the owning component's points. Marks the shape for
    // rasterization at the top of the next frame; cheap enough to call whenever
    // the geometry actually changes, and nothing else triggers the work.
    void setPath(const GPUWidgets::Path& newPath,
                 GPUWidgets::FillRule rule = GPUWidgets::FillRule::NonZero);

    // The same geometry stroked rather than filled. There is no such thing as
    // rasterizing a stroke here - the kernel fills - so the stroke is turned
    // into the region it covers and that is what the shape holds. See
    // GPUWidgets::strokeToFill, and note its warning about flatness: build the
    // path with a tighter tolerance than a fill would need.
    void setStroke(const GPUWidgets::Path& newPath,
                   const GPUWidgets::StrokeStyle& style);

    void clear();

    // True until a path has been set and rasterized. Drawing one is a no-op.
    bool isEmpty() const;

    // Where the coverage lands, in the owning component's points - the path's
    // bounds snapped out to whole device pixels. Not the path's own bounds:
    // this is the rect the quad has to cover for every partly-covered pixel to
    // be drawn.
    Rect getBounds() const;

    // The atlas rect the quad samples.
    Rect getMaskUV() const { return maskUV; }

    // True when this shape has geometry and no mask, the atlas having had no
    // room for it. It stays true until the shape is rasterized again, which is
    // what makes it a count of what is missing from the picture rather than of
    // what happened during one frame -- the frame after a drop allocates
    // nothing at all, and the shape is just as absent.
    bool wasDropped() const { return dropped; }

private:
    friend class ComponentHost;

    // Allocates a slot and records the dispatch. Called by the host, on the
    // frame's own command buffer, before the render pass opens.
    void rasterize(CoverageAtlas& atlas, float scale, GPU::ComputePass& pass);

    // The atlas moved everything, or the display did: whatever was rasterized
    // is no longer where the uv says it is, and the slot it was in belongs to
    // somebody else now.
    void invalidate()
    {
        dirty = true;
        placed = false;
    }

    bool isDirty() const { return dirty; }

    Component& owner;

    GPUWidgets::Path path;
    GPUWidgets::FillRule fillRule = GPUWidgets::FillRule::NonZero;
    GPUWidgets::PathRasterizer rasterizer;

    // The room reserved in the atlas, kept between rasterizations: a mask that
    // still fits in it stays put, which is what stops a knob being dragged from
    // consuming the atlas one frame at a time.
    CoverageAtlas::Slot slot;

    Rect maskUV;
    Rect bounds;

    bool dirty = false;
    bool ready = false;
    bool placed = false;
    bool dropped = false;
};
} // namespace eacp::UI
