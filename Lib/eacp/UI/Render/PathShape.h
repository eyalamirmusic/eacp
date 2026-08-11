#pragma once

#include "CoverageAtlas.h"
#include "MaskCache.h"

namespace eacp::UI
{
class Component;

// A vector shape, authored in the owning component's points and rasterized at
// the host's device scale. Must be a member of the component that draws it:
// setPath schedules a compute dispatch for the top of the next frame.
class PathShape
{
public:
    // Coverage is stored per device pixel, so a mask does not scale with the
    // shape it covers.
    enum class Backing
    {
        // A mask while the shape is small enough to be worth storing one.
        Automatic,

        // Exact coverage, at the cost of the shape's own area in the atlas.
        Mask,

        // A preference, not an instruction: geometry a triangulator cannot read
        // falls back to a mask.
        Mesh
    };

    // Registers with the component, which must outlive the shape.
    explicit PathShape(Component& ownerToUse);
    ~PathShape();

    PathShape(const PathShape&) = delete;
    PathShape& operator=(const PathShape&) = delete;

    // Takes effect at the next rasterization, so setting it before the path is
    // the usual order.
    void setBacking(Backing newBacking);
    Backing getBacking() const { return backing; }

    bool isMeshed() const { return !mesh.empty(); }

    // True of every copy but the first of a shape an interface repeats.
    bool isSharingMask() const { return sharing; }

    // In the owning component's points. Marks the shape for rasterization at the
    // top of the next frame; setting the same geometry again costs only a hash.
    void setPath(const GPUWidgets::Path& newPath,
                 GPUWidgets::FillRule rule = GPUWidgets::FillRule::NonZero);

    // Stored as the region the stroke covers, the kernel only being able to
    // fill. Build `newPath` with a tighter flatness than a fill would need.
    void setStroke(const GPUWidgets::Path& newPath,
                   const GPUWidgets::StrokeStyle& style);

    void clear();

    // True until a path has been set and rasterized. Drawing one is a no-op.
    bool isEmpty() const;

    // In the owning component's points, and wider than the path's own bounds:
    // the rect the quad must cover for every partly-covered pixel to be drawn.
    // A meshed shape reports the triangles' reach instead.
    Rect getBounds() const;

    // The atlas rect the quad samples.
    Rect getMaskUV() const { return maskUV; }

    // Empty for a shape backed by a mask.
    const Vector<GPUWidgets::MeshVertex>& getMesh() const { return mesh; }

    // Geometry with no mask, the atlas having had no room. Stays true until the
    // shape is rasterized again, so it counts what is missing from the picture.
    bool wasDropped() const { return dropped; }

private:
    friend class ComponentHost;

    // Allocates a slot and hands the binned path to `batch`, which the caller
    // dispatches once for the whole tree. Records no GPU work itself, and none
    // at all for a shape taking a mask somebody else published.
    void rasterize(CoverageAtlas& atlas,
                   MaskCache& cache,
                   float scale,
                   GPUWidgets::CoverageBatch& batch);

    // False when the shape is worth a mask, or the geometry is something a
    // triangulator cannot read - the mask route answers for both.
    bool buildMesh(float scale);

    // For an atlas or display that moved: the uv no longer names this shape's
    // texels, and the slot belongs to somebody else.
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

    // Meaningless while the path is empty, the one state a hash cannot describe.
    std::uint64_t geometryHash = 0;

    Backing backing = Backing::Automatic;

    // Non-empty says this shape is meshed; the two routes are exclusive, so a
    // shape holding a mesh holds no slot.
    Vector<GPUWidgets::MeshVertex> mesh;

    // Kept between rasterizations, so a mask that still fits stays put. Valid
    // only while `placed`.
    CoverageAtlas::Slot slot;

    // The key this shape offered its mask under, or zero - held so a change of
    // geometry can take the offer back.
    std::uint64_t publishedKey = 0;

    // Set once the geometry has changed under this shape, after which it stops
    // publishing: an offer it cannot take back costs a slot it could rewrite.
    bool churning = false;

    // A shape drawing a mask it was handed holds no claim on the slot.
    bool sharing = false;

    Rect maskUV;
    Rect bounds;

    bool dirty = false;
    bool ready = false;
    bool placed = false;
    bool dropped = false;
};
} // namespace eacp::UI
