#include "ShaderBuilder.h"

// Everywhere else the native shader source is GLSL, compiled to SPIR-V.

namespace eacp::GPU::detail
{
ShaderBackend nativeShaderBackend()
{
    return ShaderBackend::Vulkan;
}
} // namespace eacp::GPU::detail
