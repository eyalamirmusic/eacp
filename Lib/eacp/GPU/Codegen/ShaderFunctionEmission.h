#pragma once

#include "ShaderDialect.h"

// How each dialect spells a function the shader declares (ShaderFunction.h):
// emitted once per source, only where a stage calls it, every callee ahead of
// its callers. MSL and HLSL hold both render stages in one source, so a
// function either calls is printed once there; GLSL has a source per stage and
// prints it inside each stage's guard that calls it.
//
// HLSL and GLSL reach a buffer, a texture, a sampler and the uniform block as
// globals, so a function names them as its caller does. MSL has no globals of
// them - each is a parameter of the entry point - so a function takes the ones
// its body reaches, its callees' included, as parameters behind its own:
// `uniforms`, then texture and sampler slots, then buffer slots, in slot
// order, under the names the entry point gives them; a call passes them on.

namespace eacp::GPU::ShaderEmission
{
// The functions the roots call, directly or through other functions, each
// once and every callee ahead of its callers.
Vector<int> findCalledFunctions(const ShaderGraph& graph, const Vector<int>& roots);

// The expressions of every function the roots call, added to them: what a
// stage reaches when it is asked which uniforms, textures and buffers it
// reads, and whether it takes derivatives.
void appendFunctionBodyRoots(const ShaderGraph& graph, Vector<int>& roots);

std::string getFunctionName(const ShaderGraph& graph, int function);

// MSL's module-scope arguments of a call of `function`, in parameter order.
Vector<std::string> getMetalFunctionArguments(const ShaderGraph& graph,
                                              int function);

// The definitions of `functions`, in that order. A render program's buffers
// are all read-only, a kernel's as it declared them.
std::string printFunctionDefinitions(const ShaderGraph& graph,
                                     Backend backend,
                                     const Vector<int>& functions);
} // namespace eacp::GPU::ShaderEmission
