#pragma once

#include "ShaderDialect.h"

namespace eacp::GPU::ShaderEmission
{
// The directives a WGSL module opens with: f16 where the shader holds one, and
// for a render program the derivative uniformity check turned off, which the
// other dialects do not make (a sample in a branch on a varying is ordinary).
std::string getWgslHeader(const ShaderGraph& graph);

// The helper functions the graph calls, each defined once; one per argument
// type for those WGSL cannot overload.
std::string getWgslHelperDefinitions(const ShaderGraph& graph);

// Whether every name the shader's author chose - a struct's, its members',
// a function's, an override's - is one WGSL lets an identifier be.
bool usesOnlyWgslIdentifiers(const ShaderGraph& graph);
} // namespace eacp::GPU::ShaderEmission
