#pragma once

#include "ShaderDialect.h"

namespace eacp::GPU::ShaderEmission
{
std::string emitCompute(const ShaderGraph& graph, Backend backend);
} // namespace eacp::GPU::ShaderEmission
