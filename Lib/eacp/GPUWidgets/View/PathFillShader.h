#pragma once

#include "../Common.h"

#include "Vertices.h"

namespace eacp::GPUWidgets
{
// Draws a 2D triangle mesh in a flat colour. Positions are in viewport space:
// top-left origin, y down, as the View coordinate system is.
struct PathFillShader final : GPU::ShaderProgram
{
    PathFillShader() { compile(); }

    void define() override
    {
        auto position = vertexInput(&FillVertex::position);

        auto clipX = position.x() / (viewport.x() * 0.5f) - 1.0f;
        auto clipY = 1.0f - position.y() / (viewport.y() * 0.5f);

        setPosition(float4(clipX, clipY, 0.0f, 1.0f));
        setFragment(color);
    }

    GPU::Uniform<GPU::Float2> viewport; // logical width/height paths map into
    GPU::Uniform<GPU::Float4> color; // solid RGBA fill

    EACP_SHADER(viewport, color)
};
} // namespace eacp::GPUWidgets
