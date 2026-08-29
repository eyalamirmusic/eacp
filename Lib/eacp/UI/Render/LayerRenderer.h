#pragma once

#include "ClipMask.h"
#include "CoverageAtlas.h"
#include "Layer.h"

namespace eacp::UI
{
// A layer's texture drawn back into the pass, faded by its opacity.
//
// One draw apiece and no batching, because there is nothing to batch: every
// layer is its own texture, and two of them could not share an instanced draw
// however alike they were. Which is also why this is not a Participant -- it
// queues nothing, so there is nothing for the pass to drain when it ends. What
// it does have to do is go out *in order* with the two batching renderers, and
// that is Graphics' business: it flushes whatever they are holding before this
// draws, exactly as it does between the two of them.
//
// The texture holds colour already weighted by its own coverage -- which is what
// falls out of drawing into a transparent target, and why the layer pass uses
// BlendMode::AlphaBlendOntoTransparent. So the fragment stage divides it back
// out before the blend puts it back, which sounds circular and is not: the two
// cancel exactly, and doing it this way keeps every pipeline in the tier on one
// blend equation rather than adding a premultiplied one for this alone.
class LayerRenderer
{
public:
    LayerRenderer(Point logicalSizeToUse,
                  int sampleCountToUse,
                  GPU::PixelFormat colorFormatToUse = GPU::PixelFormat::BGRA8Unorm);

    ~LayerRenderer();

    void setLogicalSize(Point size);

    // `destination` is where the layer's own bounds land in the space this
    // draws in. A clip mask multiplies the whole layer, so a clipped group is
    // cut once rather than shape by shape.
    //
    // `destination` is where the bounds land *before* the layer's transform:
    // the matrix is read off the layer and applied inside them, so what is
    // passed here is where the content would be if it were not turned. The
    // clip is still a rect in this space, which is why a rotated layer is cut
    // by an upright scissor rather than by a turned one.
    void draw(GPU::RenderPass& pass,
              const Layer& layer,
              const Rect& destination,
              const ClipMask& clip,
              const CoverageAtlas& atlas);

private:
    struct Program;

    Point logicalSize;
    int sampleCount = 1;
    GPU::PixelFormat colorFormat = GPU::PixelFormat::BGRA8Unorm;

    OwningPointer<Program> program;
};
} // namespace eacp::UI
