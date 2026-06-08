#pragma once

#include <eacp/GPU/GPU.h>
#include <eacp/Graphics/Primitives/Primitives.h>

namespace eacp::GPUWidgets
{
// One filled vertex: just a 2D position. The triangle list a path tessellates to
// is uploaded as an array of these. A Graphics::Point is exactly a float2, so it
// is the position field directly.
struct FillVertex
{
    Graphics::Point position;
};
} // namespace eacp::GPUWidgets

// Teach the shader layer that a Graphics::Point is a float2, so it can stand in as
// the vertex position field above. Mirrors the EACP_SHADER_VALUE calls the GPU
// demos make for their own vertex types.
EACP_SHADER_VALUE(eacp::Graphics::Point, Float2)

namespace eacp::GPUWidgets
{
// The EDSL-authored shader PathView fills with. It maps a path-space position into
// clip space using the viewport size (top-left origin, y down, like the View
// coordinate system) and paints every pixel the solid fill colour.
//
// The colour is read in the vertex stage and passed to the fragment stage through
// a varying, because the GPU module binds the uniform block to the vertex stage
// only. Reusable on its own for drawing any 2D triangle mesh in a flat colour.
struct PathFillShader final : GPU::ShaderProgram
{
    GPU::Uniform<GPU::Float2> viewport; // logical width/height paths map into
    GPU::Uniform<GPU::Float4> color; // solid RGBA fill

    EACP_SHADER(viewport, color)

    PathFillShader() { compile(); }

    void define() override
    {
        auto position = vertexInput(&FillVertex::position);
        auto fragColor = varying(color);

        auto clipX = position.x() / (viewport.x() * constant(0.5f)) - constant(1.0f);
        auto clipY = constant(1.0f) - position.y() / (viewport.y() * constant(0.5f));

        setPosition(float4(clipX, clipY, constant(0.0f), constant(1.0f)));
        setFragment(fragColor);
    }
};
} // namespace eacp::GPUWidgets
