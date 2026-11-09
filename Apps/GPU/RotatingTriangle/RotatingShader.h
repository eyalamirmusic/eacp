#pragma once

#include <eacp/GPU/GPU.h>

// The triangle's shader, in a header of its own so Shaders.cpp can hand its
// graph to the build-time compiler while Main.cpp draws with it.

namespace RotatingTriangle
{
using namespace eacp;
using namespace GPU;
using namespace Maths;

// Value sub-types instead of raw float arrays. Maths::Vec2 arrives already known
// to the shader layer as a Float2; a type of your own says its shape with
// EACP_SHADER_VALUE, which is the same registration the library does for Vec2 -
// either way the field stands in wherever a float2 / float3 is expected.
struct Color
{
    float r = 0.f;
    float g = 0.f;
    float b = 0.f;
};

struct Vertex
{
    Vec2 position;
    Color color;
};
} // namespace RotatingTriangle

EACP_SHADER_VALUE(RotatingTriangle::Color, Float3)

namespace RotatingTriangle
{
// The shader is an object. The angle is a named uniform you set; the vertex inputs
// are pulled straight out of the Vertex struct in define(), so that struct is the
// single source of the vertex layout - no separate input declarations to keep in
// sync, and the layout reads the fields' real offsets.
struct RotatingShader final : ShaderProgram
{
    RotatingShader() { compile(); }

    void define() override
    {
        auto position = vertexInput(&Vertex::position);
        auto color = vertexInput(&Vertex::color);
        auto varyingColor = varying(color);

        auto c = cos(angle);
        auto s = sin(angle);
        auto px = position.x();
        auto py = position.y();
        auto rotated = float2(px * c - py * s, px * s + py * c);

        setPosition(float4(rotated, 0.0f, 1.0f));
        setFragment(float4(varyingColor, 1.0f));
    }

    Uniform<Float> angle;

    EACP_SHADER(angle)
};
} // namespace RotatingTriangle
