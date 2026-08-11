#include "ShaderBuilder.h"

#include "ShaderEmitter.h"

namespace eacp::GPU::detail
{
ShaderSource nativeShaderSource(const ShaderGraph& graph)
{
    return ShaderSource::hlsl(emitHlsl(graph));
}
} // namespace eacp::GPU::detail
