#include "ShaderBuilder.h"

#include "ShaderEmitter.h"

// Windows backend selection: the native shader source is HLSL.

namespace eacp::GPU
{
ShaderBackend nativeShaderBackend()
{
    return ShaderBackend::DirectX;
}

namespace detail
{
ShaderSource nativeShaderSource(const ShaderGraph& graph)
{
    return ShaderSource::hlsl(emitHlsl(graph));
}
} // namespace detail
} // namespace eacp::GPU
