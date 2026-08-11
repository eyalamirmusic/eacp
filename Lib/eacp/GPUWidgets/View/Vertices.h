#pragma once

#include "../Common.h"

namespace eacp::GPUWidgets
{
struct FillVertex
{
    Graphics::Point position;
};

struct GradientVertex
{
    Graphics::Point position;
    Graphics::Color color;
};
} // namespace eacp::GPUWidgets

// Registered once here: two specialisations of the same type in one translation
// unit would not compile.
EACP_SHADER_VALUE(eacp::Graphics::Point, Float2)
EACP_SHADER_VALUE(eacp::Graphics::Color, Float4)
