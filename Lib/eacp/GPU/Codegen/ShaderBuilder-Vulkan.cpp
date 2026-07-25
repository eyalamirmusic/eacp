#include "ShaderBuilder.h"

#include "ShaderEmitter.h"

// Vulkan backend selection: the native shader source is SPIR-V.

namespace eacp::GPU::detail
{
ShaderSource nativeShaderSource(const ShaderGraph& graph)
{
    return ShaderSource::spirv(emitSpirv(graph));
}
} // namespace eacp::GPU::detail
