#pragma once

#include "ClipMask.h"
#include "CoverageAtlas.h"
#include "Layer.h"

namespace eacp::UI
{
// A layer's texture drawn back into the pass, faded by its opacity. Queues
// nothing, so the caller flushes the batching renderers around it. The fragment
// stage un-premultiplies, keeping the tier on one blend equation.
class LayerRenderer
{
public:
    LayerRenderer(Point logicalSizeToUse,
                  int sampleCountToUse,
                  GPU::PixelFormat colorFormatToUse = GPU::PixelFormat::BGRA8Unorm);

    ~LayerRenderer();

    void setLogicalSize(Point size);

    // `destination` is where the layer's bounds land in the space this draws in.
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
