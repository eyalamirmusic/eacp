#include "ShaderBuilder.h"

// Windows backend selection: the native shader source is HLSL.

namespace eacp::GPU::detail
{
ShaderBackend nativeShaderBackend()
{
    return ShaderBackend::DirectX;
}
} // namespace eacp::GPU::detail
