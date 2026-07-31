#pragma once

#include "../Common.h"
#include "GradientRamps.h"

#include <eacp/GPUWidgets/GPUWidgets.h>

namespace eacp::UI
{
// Which corner of its triangle a vertex is: 0, 1 or 2. The whole per-vertex
// stream, because the geometry itself is per-instance -- see MeshTriangle.
struct MeshCorner
{
    float index;
};

// One triangle of a filled shape. Per-instance rather than per-vertex, and that
// is the reason this renderer looks the way it does: a program's vertex buffer
// is one buffer it owns and overwrites, so a second upload in a frame would land
// on geometry the first draw has not read yet, while the per-instance path
// streams through a pool that hands out a buffer nothing in flight is using. A
// batch has to be able to flush many times in one frame -- on every clip change
// and every time a masked shape is drawn between two meshed ones -- so the
// geometry goes down the path that allows it.
struct MeshTriangle
{
    float positionA[2];
    float positionB[2];
    float positionC[2];

    // How much of the shape reaches each of the three corners, with which kind
    // of gradient fills it in the fourth. A shape's interior carries 1
    // throughout and its feather ring fades to 0, so interpolating the first
    // three across the triangle is the whole of the antialiasing: no distance
    // field, and no multisampling to ask a single-sample pass for.
    float coverage[4];

    float color[4];

    // The same two the quad renderer carries, meaning the same things: the map
    // into the gradient's own space, its translation, the row of the ramp
    // texture and the spread mode.
    //
    // Per triangle rather than per shape, which is what this costs: a meshed
    // shape is a few hundred triangles, and every one of them repeats its
    // gradient. It is 32 bytes a triangle against an indirection through a
    // buffer the fragment stage would have to read, and the cheaper of those two
    // is not obvious enough to build the harder one first.
    float gradient[4];
    float gradientRamp[4];
};

// Draws filled shapes as triangles, batched and instanced.
//
// The sibling of ShapeBatch, and what a shape too large for the coverage atlas
// is drawn by instead. A mask is rasterized per device pixel and stored, so it
// costs the atlas the shape's own area on screen; a mesh is a few hundred
// triangles whatever size the shape is drawn at. Which is what a document of
// large stacked artwork needs, that being the case the atlas cannot hold -- see
// GPUWidgets::tessellateAntialiasedFill for the geometry and PathShape for the
// choice between the two.
//
// Ordering with ShapeBatch is the caller's business and cannot be dodged: two
// renderers queueing into one pass draw in flush order and not in call order, so
// whoever issues into both has to flush one before queueing into the other. That
// is what Graphics does, and the cost is one draw per alternation -- nothing at
// all for a document whose large shapes are all meshed, and a draw apiece for
// one that alternates.
//
// Joins the pass as a Participant, like every other batcher here, so whatever is
// still queued when the pass ends is drawn without a flush call to forget.
class MeshBatch : public GPU::RenderPass::Participant
{
public:
    // logicalSize is the space draws are expressed in -- the same points
    // ShapeBatch works in -- so a resize sets it rather than rebuilding
    // anything.
    MeshBatch(GradientRamps& rampsToUse,
              Point logicalSizeToUse,
              int sampleCountToUse,
              GPU::PixelFormat colorFormatToUse = GPU::PixelFormat::BGRA8Unorm);

    ~MeshBatch() override;

    void begin(GPU::RenderPass& passToUse);
    void end();
    void flush();

    void setLogicalSize(Point size);

    // A tessellated shape, in the batch's own space: `mesh` is a flat triangle
    // list authored in some component's points and `offset` is where that
    // component sits, so one tessellation serves a component wherever it moves
    // to. The colour multiplies each vertex's own coverage.
    void addMesh(const Vector<GPUWidgets::MeshVertex>& mesh,
                 Point offset,
                 const Color& color,
                 const GradientFill& gradient = {});

    bool isEmpty() const { return triangles.empty(); }

private:
    struct Program;

    void flushInto(GPU::RenderPass& endingPass) override;
    void detach();

    GradientRamps& ramps;

    Point logicalSize;
    int sampleCount = 1;
    GPU::PixelFormat colorFormat = GPU::PixelFormat::BGRA8Unorm;

    OwningPointer<Program> program;
    Vector<MeshTriangle> triangles;

    GPU::RenderPass* pass = nullptr;
};
} // namespace eacp::UI
