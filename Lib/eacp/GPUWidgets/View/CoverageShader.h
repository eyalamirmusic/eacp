#pragma once

#include "../Common.h"

#include "Vertices.h"

namespace eacp::GPUWidgets
{
// The consumer half of PathRasterizer: a quad, placed in device pixels, that
// paints a solid colour through a coverage mask.
//
// The quad is the mask's own size, so one texel lands on exactly one pixel and
// the sampler is Nearest - filtering a 1:1 mapping can only blur what the
// kernel already got right. It blends, because coverage *is* an alpha: without
// that an antialiased edge would punch its partial pixels straight through
// whatever is behind it.
struct CoverageShader final : GPU::ShaderProgram
{
    CoverageShader();

    void define() override;

    // Uploads the unit quad and builds the pipeline. Spelled apart from the
    // base prepare() because the geometry and the blend mode are not the
    // caller's to choose: a coverage quad that does not blend is not one.
    void prepareQuad(int sampleCount);

    // Draws one rasterized path. pixelRect is where the mask lands in device
    // pixels, top-left origin - PathRasterizer::getCoveredBounds() times the
    // scale it rasterized at. Set viewport once per frame first.
    void drawMask(GPU::RenderPass& pass,
                  const GPU::Texture& maskToDraw,
                  const Graphics::Rect& pixelRect,
                  const Graphics::Color& fill);

    void setViewport(float pixelWidth, float pixelHeight);

    GPU::Uniform<GPU::Float2> viewport; // device pixels
    GPU::Uniform<GPU::Float4> rect; // x, y, w, h in device pixels
    GPU::Uniform<GPU::Float4> color;
    GPU::Uniform<GPU::Texture2D> mask;

    EACP_SHADER(viewport, rect, color, mask)
};
} // namespace eacp::GPUWidgets
