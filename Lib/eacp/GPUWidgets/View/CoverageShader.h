#pragma once

#include "../Common.h"

#include "Vertices.h"

namespace eacp::GPUWidgets
{
// Paints a solid colour through a PathRasterizer coverage mask. The quad is the
// mask's own size, so sampling is Nearest at 1:1, and it must blend: coverage is
// an alpha.
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

    // Apart from the base prepare(): the geometry and blend mode are fixed.
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

    // pixelRect is in device pixels, top-left origin: getCoveredBounds() times
    // the scale it rasterized at. setViewport must have run this frame.
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
