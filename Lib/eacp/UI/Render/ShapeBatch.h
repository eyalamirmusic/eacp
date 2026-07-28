#pragma once

#include "../Common.h"
#include "CoverageAtlas.h"

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
    // The destination parallelogram, already grown by the antialiasing margin:
    // where the box's top-left lands, and where its two axes go from there. A
    // parallelogram rather than a rect so one primitive covers both axis-aligned
    // boxes and arbitrarily oriented ones, which is what a line of any angle is.
    float origin[2];
    float edgeX[2];
    float edgeY[2];

    // Half the box's *undrawn* size, in points -- what the distance field is
    // measured against -- and half the grown size the quad actually spans. The
    // two differ by the margin, which is what gives the edge somewhere to fade.
    float halfSize[2];
    float halfExtent[2];

    float color[4];

    // Corner radius, border width (both in points), and whether this is a
    // border at all. The last is a flag the CPU already knows rather than
    // something the shader rederives from the width: comparing a float against
    // zero per fragment to answer a question settled per shape is work, and the
    // fourth slot was going spare anyway.
    float shape[4];

    // The rect of the coverage atlas this shape's own coverage is multiplied
    // by, as u, v, width, height. A vector path is a box masked by the coverage
    // a kernel computed for it; everything else points at the atlas's opaque
    // texel and is multiplied by one. See CoverageAtlas for why one pipeline
    // does both.
    float mask[4];
};

// Draws rounded rectangles, borders and lines, batched and instanced.
//
// One primitive covers all of them, because to a signed distance field they are
// the same shape read three ways: a rounded box, the same box's outline, and a
// box whose corner radius is half its thickness. That matters more than the
// tidiness -- it means a fill, an outline and a line all share one pipeline, so
// a widget that draws all three does not break the batch between them.
//
// Antialiasing is analytic: the fragment's distance from the shape's edge
// becomes its coverage, so a corner is smooth at any radius and any scale
// without multisampling. Which is the point -- the pass cannot use MSAA (the
// glyph pipeline is single-sample, and a multisampled scissor edge feathers),
// so the shapes have to bring their own.
//
// Batching works the way SpriteRenderer's does, and deliberately: begin(pass)
// joins the pass as a Participant, so whatever is still queued is drawn when the
// pass ends and there is no flush call to forget.
class ShapeBatch : public GPU::RenderPass::Participant
{
public:
    // atlas is the coverage texture every shape samples -- a path for its own
    // mask, everything else for the opaque texel that multiplies by one. Taken
    // by reference rather than settable, because the fragment stage always
    // reads it: a batch without one could not draw at all, so there is no
    // useful state in which it is absent.
    //
    // logicalSize is the space draws are expressed in; pixelScale is device
    // pixels per point, which sets how wide the antialiasing ramp is. Both are
    // uniforms, so a resize sets them rather than rebuilding anything.
    ShapeBatch(const CoverageAtlas& atlasToUse,
               Point logicalSizeToUse,
               float pixelScaleToUse,
               int sampleCountToUse,
               GPU::PixelFormat colorFormatToUse = GPU::PixelFormat::BGRA8Unorm);

    ~ShapeBatch() override;

    void begin(GPU::RenderPass& passToUse);
    void end();
    void flush();

    void setLogicalSize(Point size);
    void setPixelScale(float scale);

    // Draws what is queued and then clips the pass, so that what was issued
    // before the call escapes the new clip. Render-target *pixels*, matching
    // RenderPass::setScissorRect.
    void setScissorRect(const Rect& rectInPixels);
    void clearScissorRect();

    void fillRect(const Rect& rect, const Color& color, float cornerRadius = 0.f);

    // An outline drawn inside the rect's edges.
    void drawRect(const Rect& rect,
                  const Color& color,
                  float thickness = 1.f,
                  float cornerRadius = 0.f);

    // A line of any orientation, with round caps -- which the distance field
    // gives for nothing, the cap being the corner radius of a box one thickness
    // tall.
    void drawLine(Point a, Point b, const Color& color, float thickness = 1.f);

    // A rect painted through a coverage mask: the atlas rect `maskUV` decides
    // how much of `color` each pixel gets. This is how a vector path draws, and
    // it joins the same batch as everything above it -- which is the whole
    // reason the mask lives in a shared atlas rather than a texture of its own.
    //
    // The rect is the mask's own footprint and takes no antialiasing margin: the
    // mask already carries its own soft edge, and the box around it is square.
    void fillMask(const Rect& rect, const Color& color, const Rect& maskUV);

private:
    struct Program;

    void flushInto(GPU::RenderPass& endingPass) override;
    void detach();

    // The core primitive every call above becomes: a parallelogram carrying a
    // rounded-box field of `halfSize`, grown by the antialiasing margin.
    void addShape(Point origin,
                  Point edgeX,
                  Point edgeY,
                  Point halfSize,
                  const Color& color,
                  float cornerRadius,
                  float borderWidth);

    void addAxisAlignedShape(const Rect& rect,
                             const Color& color,
                             float cornerRadius,
                             float borderWidth);

    static void setMask(ShapeInstance& instance, const Rect& maskUV);

    const CoverageAtlas& atlas;

    Point logicalSize;
    float pixelScale = 1.f;
    int sampleCount = 1;
    GPU::PixelFormat colorFormat = GPU::PixelFormat::BGRA8Unorm;

    OwningPointer<Program> program;
    Vector<ShapeInstance> instances;

    GPU::RenderPass* pass = nullptr;
};
} // namespace eacp::UI
