#include "ShaderBuilder.h"

#include "ShaderEmitter.h"

// Linux backend selection: the native shader source is GLSL 450, which
// eacp-spirv compiles to the SPIR-V the Vulkan backend will consume.

namespace eacp::GPU::detail
{
ShaderSource nativeShaderSource(const ShaderGraph& graph)
{
    return ShaderSource::glsl(emitGlsl(graph));
}
} // namespace eacp::GPU::detail
