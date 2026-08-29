#pragma once

#include "../Common.h"
#include "ClipMask.h"
#include "CoverageAtlas.h"
#include "DrawList.h"
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
    //
    // The fourth is no longer spare: it carries which kind of gradient fills
    // this shape, 0 for none.
    float shape[4];

    // The four of the affine that takes a fragment into the gradient's own
    // space, its translation living in the first two of the next field. See
    // GradientFill for why a whole matrix and not an axis.
    float gradient[4];

    // That translation, then the row of the ramp texture this shape's colours
    // were baked into, then the spread mode. See GradientRamps: every gradient
    // in the interface is a row of one texture, so a gradient-filled shape joins
    // the same instanced draw as a flat one.
    float gradientRamp[4];

    // The rect of the coverage atlas this shape's own coverage is multiplied
    // by, as u, v, width, height. A vector path is a box masked by the coverage
    // a kernel computed for it; everything else points at the atlas's opaque
    // texel and is multiplied by one. See CoverageAtlas for why one pipeline
    // does both.
    float mask[4];

    // How wide this shape's edge ramp is -- one over twice the blur radius --
    // and whether it has one at all. A shadow is this same distance field read
    // through a ramp as wide as its blur instead of one as wide as a pixel, so
    // it is two numbers on the instance rather than a pipeline of its own.
    float blur[2];

    // Where the box that cast this shadow sits inside it -- its centre against
    // this one's, and how much larger it is on every side -- and whether the
    // shadow falls inside that box rather than outside it. The shadow is not
    // drawn where the box is, which is what CSS means by casting a shadow as
    // though the box were opaque.
    float caster[4];
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
               GradientRamps& rampsToUse,
               Point logicalSizeToUse,
               float pixelScaleToUse,
               int sampleCountToUse,
               GPU::PixelFormat colorFormatToUse = GPU::PixelFormat::BGRA8Unorm);

    ~ShapeBatch() override;

    void begin(GPU::RenderPass& passToUse);
    void end();
    void flush();

    // Whether a flush would draw anything, which is what a caller ordering this
    // renderer against another one has to know: see MeshBatch.
    bool isEmpty() const { return instances.empty(); }

    // The texture every shape's coverage lives in, which a caller drawing
    // something else under the same clip needs: a clip is a rect of this atlas
    // however the thing it cuts was drawn. See LayerRenderer.
    const CoverageAtlas& getAtlas() const { return atlas; }

    void setLogicalSize(Point size);
    void setPixelScale(float scale);

    // Draws what is queued and then clips the pass, so that what was issued
    // before the call escapes the new clip. Render-target *pixels*, matching
    // RenderPass::setScissorRect.
    void setScissorRect(const Rect& rectInPixels);
    void clearScissorRect();

    // Multiplies everything queued after the call by a second mask out of the
    // same atlas, in the batch's own space. Draws what is queued first, for the
    // reason the scissor does: the clip is one uniform for the whole draw, so a
    // change of it is a batch break. An empty mask is no clip at all.
    void setClipMask(const ClipMask& mask);

    // Every call below takes the fill twice over: a colour, and a gradient that
    // replaces it where there is one. Both rather than one of the two, because
    // a gradient the ramps had no room for falls back to the colour beside it --
    // so a shape always has something to be drawn in.
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

    // A line of any orientation, with round caps -- which the distance field
    // gives for nothing, the cap being the corner radius of a box one thickness
    // tall.
    void drawLine(Point a,
                  Point b,
                  const Color& color,
                  float thickness = 1.f,
                  const GradientFill& gradient = {});

    // The shadow a box casts: the same rounded-box field, read through a ramp
    // as wide as the blur rather than as wide as a pixel, and cut back out of
    // the box itself -- outside it for an outer shadow, inside it for an inset
    // one.
    //
    // The ramp is a smoothstep across twice the blur, which is the cheapest
    // curve that reads as a Gaussian of half the radius -- the blur CSS asks
    // for -- and costs the field it already computes plus a few instructions.
    void fillShadow(const ShadowShape& shadow,
                    const Color& color,
                    const GradientFill& gradient = {});

    // A rect painted through a coverage mask: the atlas rect `maskUV` decides
    // how much of `color` each pixel gets. This is how a vector path draws, and
    // it joins the same batch as everything above it -- which is the whole
    // reason the mask lives in a shared atlas rather than a texture of its own.
    //
    // The rect is the mask's own footprint and takes no antialiasing margin: the
    // mask already carries its own soft edge, and the box around it is square.
    void fillMask(const Rect& rect,
                  const Color& color,
                  const Rect& maskUV,
                  const GradientFill& gradient = {});

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
