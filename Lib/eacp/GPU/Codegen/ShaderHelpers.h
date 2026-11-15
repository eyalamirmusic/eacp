#pragma once

#include "ShaderDialect.h"

namespace eacp::GPU::ShaderEmission
{
std::string getSourcePreamble(const ShaderGraph& graph, Backend backend);

// HLSL's GetDimensions as one expression per texture kind a shader queries;
// empty elsewhere. Defined in ShaderTexturePrinter.cpp.
std::string getTextureQueryHelpers(const ShaderGraph& graph, Backend backend);
} // namespace eacp::GPU::ShaderEmission
