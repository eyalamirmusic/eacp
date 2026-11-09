#pragma once

/** @file Emitters that turn a ShaderGraph into Metal, HLSL or GLSL source on any
    host.
*/

#include "../Common.h"

#include "ShaderGraph.h"

#include <cstdint>

namespace eacp::GPU
{
// Emit native shader source for a graph. All three dialects are produced by one
// shared walker, so they stay in lockstep by construction. Pure string
// generation with no platform APIs, so all three can be produced and tested on
// any host regardless of which one the platform actually compiles.
std::string emitMetal(const ShaderGraph& graph);
std::string emitHlsl(const ShaderGraph& graph);

// GLSL 450 for Vulkan. Both render stages share one string, each behind
// `#ifdef EACP_VERTEX` / `EACP_FRAGMENT`; a kernel has one unguarded main().
std::string emitGlsl(const ShaderGraph& graph);

// Whether a stage's expressions read a uniform at all - the same answer the
// emitter declares the Metal function parameter from, so a bind cannot disagree
// with the signature it is aimed at. GeneratedShader carries both so
// RenderPass::draw(program) binds the block to the stage that reads it and not
// to the one that does not, which the validation layer reports as an unused
// binding. The HLSL cbuffer is a global both functions already see, so there
// the answer governs the bind alone.
bool vertexReadsUniforms(const ShaderGraph& graph);
bool fragmentReadsUniforms(const ShaderGraph& graph);

// The texture and sampler slots the vertex stage reads, one bit each - the
// ones the Metal vertex function declares, and so the ones a bind has to reach
// the vertex stage with.
std::uint32_t vertexTextureSlots(const ShaderGraph& graph);

// Every node a render stage evaluates: its outputs, the expressions of its
// statements and of every function it calls, and the elements of the constant
// arrays it subscripts, stopping at a varying read, past which the value is
// the vertex stage's. What the questions above are answered from, and what
// reflection finds a stage's resources in.
Vector<int> vertexStageNodes(const ShaderGraph& graph);
Vector<int> fragmentStageNodes(const ShaderGraph& graph);

// One varying the emitted render stages pass, at its place in location order:
// the declared ones first, then each vertex input the fragment stage reads
// directly, carried at its type's default interpolation - `attribute` is that
// input, -1 for a declared varying.
struct EmittedVarying
{
    ValueType type = ValueType::Float;
    Interpolation interpolation;
    int attribute = -1;
};

Vector<EmittedVarying> emittedVaryings(const ShaderGraph& graph);

// Whether the vertex stage reaches anything that takes its level from the
// fragment's derivatives: sample with no level, sampleBias, sampleCompare, a
// layered sample, dfdx, dfdy or fwidth. WGSL allows them in the fragment stage
// alone, and the emitter asserts the vertex stage has none.
bool vertexUsesImplicitDerivatives(const ShaderGraph& graph);

// Whether each render stage reads only the variables it declared: a vertex
// one reaches the fragment through a varying, never directly. The emitter
// asserts it.
bool stagesReadOnlyTheirOwnVariables(const ShaderGraph& graph);
} // namespace eacp::GPU
