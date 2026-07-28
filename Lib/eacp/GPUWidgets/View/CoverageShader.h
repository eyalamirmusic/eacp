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
    CoverageShader()
    {
        mask.sampling = {GPU::TextureFilter::Nearest,
                         GPU::TextureAddressMode::Clamp};
        compile();
    }

    void define() override
    {
        auto corner = vertexInput(&FillVertex::position);

        auto pixelX = rect.x() + corner.x() * rect.z();
        auto pixelY = rect.y() + corner.y() * rect.w();

        setPosition(float4(pixelX / (viewport.x() * 0.5f) - 1.f,
                           1.f - pixelY / (viewport.y() * 0.5f),
                           0.f,
                           1.f));

        auto uv = varying(corner);
        setFragment(float4(color.xyz(), color.w() * sample(mask, uv).x()));
    }

    // Uploads the unit quad and builds the pipeline. Spelled apart from the
    // base prepare() because the geometry and the blend mode are not the
    // caller's to choose: a coverage quad that does not blend is not one.
    void prepareQuad(int sampleCount)
    {
        static const FillVertex quad[] = {
            {{0.f, 0.f}},
            {{1.f, 0.f}},
            {{0.f, 1.f}},
            {{1.f, 0.f}},
            {{1.f, 1.f}},
            {{0.f, 1.f}},
        };

        setVertices(quad);
        prepare(sampleCount,
                false,
                GPU::PrimitiveTopology::Triangles,
                GPU::BlendMode::AlphaBlend);
    }

    // Draws one rasterized path. pixelRect is where the mask lands in device
    // pixels, top-left origin - PathRasterizer::getCoveredBounds() times the
    // scale it rasterized at. Set viewport once per frame first.
    void drawMask(GPU::RenderPass& pass,
                  const GPU::Texture& maskToDraw,
                  const Graphics::Rect& pixelRect,
                  const Graphics::Color& fill)
    {
        rect = Array<float, 4> {pixelRect.x, pixelRect.y, pixelRect.w, pixelRect.h};
        color = fill;
        mask = maskToDraw;

        pass.draw(*this);
    }

    void setViewport(float pixelWidth, float pixelHeight)
    {
        viewport = Array<float, 2> {pixelWidth, pixelHeight};
    }

    GPU::Uniform<GPU::Float2> viewport; // device pixels
    GPU::Uniform<GPU::Float4> rect; // x, y, w, h in device pixels
    GPU::Uniform<GPU::Float4> color;
    GPU::Uniform<GPU::Texture2D> mask;

    EACP_SHADER(viewport, rect, color, mask)
};
} // namespace eacp::GPUWidgets
