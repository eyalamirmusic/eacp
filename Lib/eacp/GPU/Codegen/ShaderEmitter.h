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

// Whether a stage's expressions read a uniform at all - the same answer the
// emitter declares the Metal function parameter from, so a bind cannot disagree
// with the signature it is aimed at. GeneratedShader carries both so
// RenderPass::draw(program) binds the block to the stage that reads it and not
// to the one that does not, which the validation layer reports as an unused
// binding. The HLSL cbuffer is a global both functions already see, so there
// the answer governs the bind alone.
bool vertexReadsUniforms(const ShaderGraph& graph);
bool fragmentReadsUniforms(const ShaderGraph& graph);
} // namespace eacp::GPU
