#pragma once

#include <string>

namespace eacp::GPU
{
class ShaderGraph;

// Emit native shader source for a graph. Both backends are produced by one
// shared walker, so they stay in lockstep by construction. Pure string
// generation with no platform APIs, so both can be produced and tested on any
// host regardless of which one the platform actually compiles.
std::string emitMetal(const ShaderGraph& graph);
std::string emitHlsl(const ShaderGraph& graph);
} // namespace eacp::GPU
