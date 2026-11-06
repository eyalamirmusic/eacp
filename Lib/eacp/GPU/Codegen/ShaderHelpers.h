#pragma once

#include "ShaderDialect.h"

namespace eacp::GPU::ShaderEmission
{
// The definitions of the helper functions a graph calls, in the dialect's
// spelling, ahead of the shader body.
std::string helperDefinitions(const ShaderGraph& graph, Backend backend);

// What every emitted source opens with: the dialect's own header lines, then
// the helper definitions.
std::string sourcePreamble(const ShaderGraph& graph, Backend backend);
} // namespace eacp::GPU::ShaderEmission
