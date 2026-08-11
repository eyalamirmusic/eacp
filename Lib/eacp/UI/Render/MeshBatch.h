#pragma once

#include "../Common.h"
#include "ClipMask.h"
#include "CoverageAtlas.h"
#include "GradientRamps.h"

#include <eacp/GPUWidgets/GPUWidgets.h>

namespace eacp::UI
{
// Which corner of its triangle a vertex is: 0, 1 or 2. The whole per-vertex
// stream, the geometry itself being per-instance.
struct MeshCorner
{
    float index;
};

// One triangle of a filled shape. Per-instance rather than per-vertex, so that
// one batch can flush many times per frame: a program's single vertex buffer
// would be overwritten under a draw that had not read it yet.
struct MeshTriangle
{
    float positionA[2];
    float positionB[2];
    float positionC[2];

    // Per corner, with the gradient kind in the fourth slot. Interpolating these
    // is the whole of the antialiasing - no distance field, no multisampling.
    float coverage[4];

    float color[4];

    // The same two the quad renderer carries, repeated per triangle rather than
    // indirected through a buffer the fragment stage would read.
    float gradient[4];
    float gradientRamp[4];
};

// Draws shapes too large for the coverage atlas as triangles. The caller must
// flush this or ShapeBatch before queueing into the other, two renderers sharing
// a pass drawing in flush order rather than call order.
class MeshBatch : public GPU::RenderPass::Participant
{
public:
    // `logicalSizeToUse` is in the same points ShapeBatch works in. The atlas is
    // needed for the clip alone, and has to be the one the quads read.
    MeshBatch(const CoverageAtlas& atlasToUse,
              GradientRamps& rampsToUse,
              Point logicalSizeToUse,
              int sampleCountToUse,
              GPU::PixelFormat colorFormatToUse = GPU::PixelFormat::BGRA8Unorm);

    ~MeshBatch() override;

    void begin(GPU::RenderPass& passToUse);
    void end();
    void flush();

    void setLogicalSize(Point size);

    // Draws what is queued. Both renderers must be told, so a clipped group
    // holding shapes of both kinds is cut identically.
    void setClipMask(const ClipMask& mask);

    // `mesh` is a flat triangle list in some component's points and `offset` is
    // where that component sits. The colour multiplies each vertex's coverage.
    void addMesh(const Vector<GPUWidgets::MeshVertex>& mesh,
                 Point offset,
                 const Color& color,
                 const GradientFill& gradient = {});

    bool isEmpty() const { return triangles.empty(); }

private:
    struct Program;

    void flushInto(GPU::RenderPass& endingPass) override;
    void detach();

    const CoverageAtlas& atlas;
    GradientRamps& ramps;

    ClipMask clip;

    Point logicalSize;
    int sampleCount = 1;
    GPU::PixelFormat colorFormat = GPU::PixelFormat::BGRA8Unorm;

    OwningPointer<Program> program;
    Vector<MeshTriangle> triangles;

    GPU::RenderPass* pass = nullptr;
};
} // namespace eacp::UI
