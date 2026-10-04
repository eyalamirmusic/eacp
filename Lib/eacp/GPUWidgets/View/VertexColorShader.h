#pragma once

#include "../Common.h"

#include "Vertices.h"

namespace eacp::GPUWidgets
{
// Fills a 2D triangle mesh with per-vertex colour, interpolated across each
// triangle. PathView uses it for gradient fills, baking a sampled gradient colour
// into each vertex. The position maps to clip space exactly like PathFillShader;
// the colour comes from the vertex rather than a uniform, so it varies across the
// surface.
struct VertexColorShader final : GPU::ShaderProgram
{
    VertexColorShader();

    void define() override;

    GPU::Uniform<GPU::Float2> viewport; // logical width/height paths map into

    EACP_SHADER(viewport)
};
} // namespace eacp::GPUWidgets
