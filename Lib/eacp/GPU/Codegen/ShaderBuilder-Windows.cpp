#include "ShaderBuilder.h"

#include "ShaderEmitter.h"

// Windows backend selection: the native shader source is HLSL.

namespace eacp::GPU::detail
{
ShaderSource nativeShaderSource(const ShaderGraph& graph)
{
    return ShaderSource::hlsl(emitHlsl(graph));
}
} // namespace eacp::GPU::detail
