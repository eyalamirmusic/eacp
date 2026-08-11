#pragma once

#include "../Common.h"
#include "ClipMask.h"
#include "CoverageAtlas.h"
#include "GradientRamps.h"

namespace eacp::UI
{
// A unit-quad corner, each component 0 or 1, mapped onto every shape's box.
struct ShapeVertex
{
    float corner[2];
};

// One shape. Everything that varies from shape to shape lives here, so a run of
// them is a single instanced draw rather than a draw apiece.
struct ShapeInstance
{
    // The destination parallelogram, already grown by the antialiasing margin -
    // a parallelogram so one primitive also covers a line of any angle.
    float origin[2];
    float edgeX[2];
    float edgeY[2];

    // Half the box's undrawn size in points, what the distance field measures
    // against, and half the grown size the quad spans. They differ by the margin.
    float halfSize[2];
    float halfExtent[2];

    float color[4];

    // Corner radius, border width (both points), whether this is a border, and
    // which kind of gradient fills it (0 for none).
    float shape[4];

    // The four of the affine into the gradient's own space, its translation
    // living in the first two of the next field.
    float gradient[4];

    // That translation, then the ramp texture row, then the spread mode.
    float gradientRamp[4];

    // The coverage atlas rect this shape is multiplied by, as u, v, width,
    // height. Everything but a path points at the atlas's opaque texel.
    float mask[4];
};

// Draws rounded rectangles, borders and lines, batched and instanced - one
// primitive and one pipeline, all three being one signed distance field read
// three ways. Antialiasing is analytic, the pass being unable to use MSAA.
class ShapeBatch : public GPU::RenderPass::Participant
{
public:
    // `logicalSizeToUse` is the space draws are expressed in; `pixelScaleToUse`
    // is device pixels per point, setting how wide the antialiasing ramp is.
    // Both are uniforms, so a resize sets them rather than rebuilding anything.
    ShapeBatch(const CoverageAtlas& atlasToUse,
               GradientRamps& rampsToUse,
               Point logicalSizeToUse,
               float pixelScaleToUse,
               int sampleCountToUse,
               GPU::PixelFormat colorFormatToUse = GPU::PixelFormat::BGRA8Unorm);

    ~ShapeBatch() override;

    void begin(GPU::RenderPass& passToUse);
    void end();
    void flush();

    // Whether a flush would draw anything, which a caller ordering this renderer
    // against another one has to know.
    bool isEmpty() const { return instances.empty(); }

    // A clip is a rect of this atlas however the thing it cuts was drawn.
    const CoverageAtlas& getAtlas() const { return atlas; }

    void setLogicalSize(Point size);
    void setPixelScale(float scale);

    // Draws what is queued first, so it escapes the new clip. Render-target
    // pixels, matching RenderPass::setScissorRect.
    void setScissorRect(const Rect& rectInPixels);
    void clearScissorRect();

    // A second mask out of the same atlas, in the batch's own space; empty is no
    // clip at all. Draws what is queued first, being one uniform per draw.
    void setClipMask(const ClipMask& mask);

    // The gradient replaces the colour where there is one; a gradient the ramps
    // had no room for falls back to it.
    void fillRect(const Rect& rect,
                  const Color& color,
                  float cornerRadius = 0.f,
                  const GradientFill& gradient = {});

    // An outline drawn inside the rect's edges.
    void drawRect(const Rect& rect,
                  const Color& color,
                  float thickness = 1.f,
                  float cornerRadius = 0.f,
                  const GradientFill& gradient = {});

    // With round caps.
    void drawLine(Point a,
                  Point b,
                  const Color& color,
                  float thickness = 1.f,
                  const GradientFill& gradient = {});

    // A rect painted through the atlas rect `maskUV`, which is how a vector path
    // draws. `rect` is the mask's own footprint and takes no margin.
    void fillMask(const Rect& rect,
                  const Color& color,
                  const Rect& maskUV,
                  const GradientFill& gradient = {});

private:
    struct Program;

    void flushInto(GPU::RenderPass& endingPass) override;
    void detach();

    // A parallelogram carrying a rounded-box field of `halfSize`, grown by the
    // antialiasing margin.
    void addShape(Point origin,
                  Point edgeX,
                  Point edgeY,
                  Point halfSize,
                  const Color& color,
                  float cornerRadius,
                  float borderWidth,
                  const GradientFill& gradient);

    void addAxisAlignedShape(const Rect& rect,
                             const Color& color,
                             float cornerRadius,
                             float borderWidth,
                             const GradientFill& gradient);

    static void setMask(ShapeInstance& instance, const Rect& maskUV);
    static void setGradient(ShapeInstance& instance, const GradientFill& gradient);

    const CoverageAtlas& atlas;
    GradientRamps& ramps;

    ClipMask clip;

    Point logicalSize;
    float pixelScale = 1.f;
    int sampleCount = 1;
    GPU::PixelFormat colorFormat = GPU::PixelFormat::BGRA8Unorm;

    OwningPointer<Program> program;
    Vector<ShapeInstance> instances;

    GPU::RenderPass* pass = nullptr;
};
} // namespace eacp::UI
