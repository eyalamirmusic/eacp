#pragma once

#include "Vertices.h"

#include <eacp/GPU/GPU.h>

namespace eacp::GPUWidgets
{
// Fills a 2D triangle mesh with per-vertex colour, interpolated across each
// triangle. PathView uses it for gradient fills, baking a sampled gradient colour
// into each vertex. The position maps to clip space exactly like PathFillShader;
// the colour comes from the vertex rather than a uniform, so it varies across the
// surface.
struct VertexColorShader final : GPU::ShaderProgram
{
    GPU::Uniform<GPU::Float2> viewport; // logical width/height paths map into

    EACP_SHADER(viewport)

    VertexColorShader() { compile(); }

    void define() override
    {
        auto position = vertexInput(&GradientVertex::position);
        auto color = vertexInput(&GradientVertex::color);
        auto fragColor = varying(color);

        auto clipX = position.x() / (viewport.x() * constant(0.5f)) - constant(1.0f);
        auto clipY = constant(1.0f) - position.y() / (viewport.y() * constant(0.5f));

        setPosition(float4(clipX, clipY, constant(0.0f), constant(1.0f)));
        setFragment(fragColor);
    }
};
} // namespace eacp::GPUWidgets
