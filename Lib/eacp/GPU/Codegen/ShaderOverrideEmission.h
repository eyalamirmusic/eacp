#pragma once

#include "../Shader/PipelineConstants.h"
#include "ShaderDialect.h"

// How each dialect declares a pipeline-overridable constant (OverrideValues.h)
// so that the pipeline, not the source, decides its value:
//
// - MSL: a function constant at index id. With a default, the constant the
//   body reads is is_function_constant_defined(it) ? it : default, so a
//   pipeline that sets no value gets the default.
// - GLSL: a specialization constant, layout(constant_id = id), initialised to
//   the default - or to the type's zero when there is none, since GLSL needs
//   one; a pipeline always gives such an override a value.
// - HLSL: FXC has no specialisation, so the value is the macro
//   EACP_OVERRIDE_<id>, defined to the default under #ifndef; a pipeline with
//   other values compiles the source again with the macros defined
//   (hlslDefinesFor). The default of one with none is the type's zero, so the
//   source as emitted still compiles, which precompiling it needs.
//
// The body reads override_<name> in all three.

namespace eacp::GPU::ShaderEmission
{
std::string getOverrideDeclarations(const ShaderGraph& graph, Backend backend);

std::string getOverrideName(const ShaderGraph& graph, int id);

// The value as a literal of its type in the dialect.
std::string printOverrideLiteral(Backend backend, const PipelineConstant& value);
} // namespace eacp::GPU::ShaderEmission
