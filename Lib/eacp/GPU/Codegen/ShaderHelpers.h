#pragma once

#include "ShaderDialect.h"

namespace eacp::GPU::ShaderEmission
{
std::string getSourcePreamble(const ShaderGraph& graph, Backend backend);
} // namespace eacp::GPU::ShaderEmission
