#pragma once

#include "../Common.h"

namespace eacp::GPU
{
class ShaderGraph;

// Pure string generation with no platform APIs, so either backend can be
// emitted and tested on any host.
std::string emitMetal(const ShaderGraph& graph);
std::string emitHlsl(const ShaderGraph& graph);

// Whether a stage's expressions read a uniform at all - the same answer the
// Metal function signature is declared from, so a bind cannot disagree with it.
bool vertexReadsUniforms(const ShaderGraph& graph);
bool fragmentReadsUniforms(const ShaderGraph& graph);
} // namespace eacp::GPU
