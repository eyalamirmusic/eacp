#pragma once

#include "../Common.h"

#include "Vertices.h"

namespace eacp::GPUWidgets
{
// PathFillShader with the colour taken per vertex and interpolated, which is how
// PathView draws a gradient.
struct VertexColorShader final : GPU::ShaderProgram
{
    VertexColorShader() { compile(); }

    void define() override
    {
        auto position = vertexInput(&GradientVertex::position);
        auto color = vertexInput(&GradientVertex::color);
        auto fragColor = varying(color);

        auto clipX = position.x() / (viewport.x() * 0.5f) - 1.0f;
        auto clipY = 1.0f - position.y() / (viewport.y() * 0.5f);

        setPosition(float4(clipX, clipY, 0.0f, 1.0f));
        setFragment(fragColor);
    }

    GPU::Uniform<GPU::Float2> viewport; // logical width/height paths map into

    EACP_SHADER(viewport)
};
} // namespace eacp::GPUWidgets
