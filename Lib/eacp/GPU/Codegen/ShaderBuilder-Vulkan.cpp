#include "ShaderBuilder.h"

#include "ShaderEmitter.h"

// Vulkan backend selection: the native shader source is SPIR-V.

namespace eacp::GPU
{
ShaderBackend nativeShaderBackend()
{
    return ShaderBackend::Vulkan;
}

namespace detail
{
ShaderSource nativeShaderSource(const ShaderGraph& graph)
{
    return ShaderSource::spirv(emitSpirv(graph));
}
} // namespace detail
} // namespace eacp::GPU
