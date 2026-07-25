#pragma once

#include "../Common.h"

namespace eacp::GPU
{
class ShaderGraph;

// Emit native shader source for a graph. Both backends are produced by one
// shared walker, so they stay in lockstep by construction. Pure string
// generation with no platform APIs, so both can be produced and tested on any
// host regardless of which one the platform actually compiles.
std::string emitMetal(const ShaderGraph& graph);
std::string emitHlsl(const ShaderGraph& graph);

// Vulkan's backend takes SPIR-V and nothing else: unlike Metal and D3D there is
// no in-driver compiler for a text dialect, so this emitter produces the binary
// directly rather than a third language for someone else to compile. That keeps
// the framework's no-third-party-dependencies position -- linking glslang to
// translate an intermediate GLSL would cost megabytes to reach the same words.
//
// Feasible because the graph is a small, closed language: no loops, no branching
// beyond discardBelow and a kernel's bounds guard, and already in SSA shape,
// which is exactly what SPIR-V wants. A render graph lands both stages in one
// module; a compute graph emits a GLCompute kernel instead. Entry points named
// as the ShaderSource says. Pure word generation with no Vulkan API calls, so it
// can be produced and tested on any host, like its two siblings above.
Vector<std::uint32_t> emitSpirv(const ShaderGraph& graph);
} // namespace eacp::GPU
