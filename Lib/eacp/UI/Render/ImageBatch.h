#pragma once

#include "../Common.h"
#include "ClipMask.h"
#include "CoverageAtlas.h"

namespace eacp::UI
{
// A unit-quad corner, each component 0 or 1, mapped onto every image's box.
struct ImageVertex
{
    float corner[2];
};

// One image. Everything that differs from quad to quad lives here, so a run of
// them out of one texture is a single instanced draw rather than a draw apiece.
struct ImageInstance
{
    // The destination parallelogram in points: where the sampled rect's
    // top-left lands, and where its two axes go from there.
    float origin[2];
    float edgeX[2];
    float edgeY[2];

    // The sampled sub-rect, in normalised texture coordinates.
    float uv0[2];
    float uv1[2];

    // Multiplied into the sample. Opaque white draws the image as it is; the
    // alpha is where an opacity goes.
    float tint[4];
};

// Draws images as textured quads, batched and instanced by texture.
//
// The third of the tier's queueing renderers, beside the shapes and the meshes,
// and the one Sprites::SpriteRenderer is the model for: a quad joins the open
// run while it samples the texture the run does, and anything else is a draw of
// its own with the run so far going out first. So a change of texture costs
// what a change of clip costs -- one draw -- and a row of icons out of one
// image is one draw however long the row.
//
// It reads the same clip the shapes do, out of the same atlas, so an image
// under a rounded clip is cut by the shape and not by its box. Sampled linear
// and through the mip chain: a picture drawn below its own size reads the level
// nearest that size rather than a scatter of texels, which is what stops a
// photo shimmering as the column holding it resizes.
//
// Ordering against the other two is the caller's business, exactly as it is
// between them: the queues go out in flush order, so whoever issues into more
// than one flushes the others first. DrawPlayer does, and counts it.
class ImageBatch : public GPU::RenderPass::Participant
{
public:
    // atlas is where a clip's coverage lives, and every image samples it --
    // an unclipped one reads the opaque texel and multiplies by one.
    ImageBatch(const CoverageAtlas& atlasToUse,
               Point logicalSizeToUse,
               int sampleCountToUse,
               GPU::PixelFormat colorFormatToUse = GPU::PixelFormat::BGRA8Unorm);

    ~ImageBatch() override;

    void begin(GPU::RenderPass& passToUse);
    void end();
    void flush();

    // Whether a flush would draw anything, which is what a caller ordering this
    // renderer against another one has to know.
    bool isEmpty() const { return instances.empty(); }

    // How many draws this batch has issued since begin(): one per run of quads
    // out of one texture. The figure that says what a screen of pictures costs
    // beyond its shapes.
    int getDrawCount() const { return draws; }

    void setLogicalSize(Point size);

    // Draws what is queued and then clips the pass, so that what was issued
    // before the call escapes the new clip. Render-target *pixels*, matching
    // RenderPass::setScissorRect.
    void setScissorRect(const Rect& rectInPixels);
    void clearScissorRect();

    // Multiplies everything queued after the call by a mask out of the atlas,
    // in the batch's own space. Draws what is queued first, since the clip is
    // one uniform for the whole draw. An empty mask is no clip at all.
    void setClipMask(const ClipMask& mask);

    // The `uv` rect of `texture`, in normalised coordinates, stretched to
    // `destination` and multiplied by `tint`. The texture is referred to and
    // not retained, so it has to outlive the flush -- which the recording that
    // holds it sees to.
    void draw(const GPU::Texture& texture,
              const Rect& destination,
              const Rect& uv,
              const Color& tint);

private:
    struct Program;

    void flushInto(GPU::RenderPass& endingPass) override;
    void detach();

    const CoverageAtlas& atlas;

    ClipMask clip;

    Point logicalSize;
    int sampleCount = 1;
    GPU::PixelFormat colorFormat = GPU::PixelFormat::BGRA8Unorm;

    OwningPointer<Program> program;

    // The open run, and the texture all of it samples.
    Vector<ImageInstance> instances;
    const GPU::Texture* batchTexture = nullptr;

    int draws = 0;

    GPU::RenderPass* pass = nullptr;
};
} // namespace eacp::UI
