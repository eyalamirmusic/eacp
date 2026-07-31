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
    // How the shape's coverage is produced. Two answers, because a mask does not
    // scale with the shape it covers: coverage is computed per device pixel and
    // stored, so a shape the size of a document costs the atlas a document's
    // worth of texels. An interface never notices - its shapes are widget-sized
    // and tile rather than stack - and artwork does: a drawing of large stacked
    // shapes asks for several times the atlas and loses whatever arrives after
    // it is full.
    enum class Backing
    {
        // A mask while the shape is small enough to be worth storing one, and a
        // mesh once it is not. What a shape gets unless it is told otherwise,
        // since the threshold is about the atlas rather than about the widget.
        Automatic,

        // Always a mask: exact coverage, at the cost of the shape's own area in
        // the atlas. What a shape wants when its edge is the point of it.
        Mask,

        // Always a mesh where the geometry allows one. A triangulator cannot
        // read every path -- holes, self-crossing contours -- and this is a
        // preference rather than an instruction, so a shape it cannot tessellate
        // falls back to a mask and draws correctly rather than not at all.
        Mesh
    };

    // Registers with the component that draws it, which is how the host finds
    // it to rasterize. The component has to outlive the shape, which holding it
    // as a member gets you.
    explicit PathShape(Component& ownerToUse);
    ~PathShape();

    PathShape(const PathShape&) = delete;
    PathShape& operator=(const PathShape&) = delete;

    // Takes effect at the next rasterization, so setting it before the path is
    // the usual order.
    void setBacking(Backing newBacking);
    Backing getBacking() const { return backing; }

    // Whether this shape ended up as triangles rather than a mask -- which is
    // worth being able to read, because it is the difference between costing the
    // atlas nothing and costing it the shape's whole area.
    bool isMeshed() const { return !mesh.empty(); }

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
    // be drawn. A meshed shape reports the triangles' own reach instead, which
    // is the path's bounds plus half the feather.
    Rect getBounds() const;

    // The atlas rect the quad samples.
    Rect getMaskUV() const { return maskUV; }

    // The triangles, in the owning component's points, for a shape that ended up
    // meshed. Empty for one backed by a mask.
    const Vector<GPUWidgets::MeshVertex>& getMesh() const { return mesh; }

    // True when this shape has geometry and no mask, the atlas having had no
    // room for it. It stays true until the shape is rasterized again, which is
    // what makes it a count of what is missing from the picture rather than of
    // what happened during one frame -- the frame after a drop allocates
    // nothing at all, and the shape is just as absent.
    bool wasDropped() const { return dropped; }

private:
    friend class ComponentHost;

    // Allocates a slot and hands the binned path to the frame's batch. No GPU
    // work is recorded here: the batch is dispatched once, after the whole tree
    // has been walked, which is what keeps a hundred paths to one dispatch.
    void rasterize(CoverageAtlas& atlas,
                   float scale,
                   GPUWidgets::CoverageBatch& batch);

    // The mesh route, tried first when the backing asks for it. False when the
    // shape is small enough to be worth a mask, or when the geometry is
    // something a triangulator cannot read - either way the mask route answers
    // for it, so a refusal here costs the atlas rather than the picture.
    bool buildMesh(float scale);

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

    Backing backing = Backing::Automatic;

    // The triangles, when this shape is meshed. Non-empty is what says it is:
    // the two routes are exclusive, so a shape holding a mesh holds no slot.
    Vector<GPUWidgets::MeshVertex> mesh;

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
