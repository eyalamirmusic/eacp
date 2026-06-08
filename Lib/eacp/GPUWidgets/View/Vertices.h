#pragma once

#include <eacp/GPU/GPU.h>
#include <eacp/Graphics/Primitives/Primitives.h>

namespace eacp::GPUWidgets
{
// Position-only vertex, used for solid fills and strokes. A Graphics::Point is
// exactly a float2, so it is the position field directly.
struct FillVertex
{
    Graphics::Point position;
};

// Position + RGBA colour vertex, used for gradient fills: PathView bakes a sampled
// gradient colour into each vertex and the shader interpolates it across each
// triangle.
struct GradientVertex
{
    Graphics::Point position;
    Graphics::Color color;
};
} // namespace eacp::GPUWidgets

// Teach the shader layer that a Graphics::Point is a float2 and a Graphics::Color
// is a float4, so they stand in as vertex fields above. Registered once here so
// every shader header can reuse the vertex types without re-specialising (two
// specialisations of the same type in one translation unit would not compile).
EACP_SHADER_VALUE(eacp::Graphics::Point, Float2)
EACP_SHADER_VALUE(eacp::Graphics::Color, Float4)
