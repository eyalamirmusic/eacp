#include "ShaderBuilder.h"

// The browser's shader source is WGSL, compiled by WebGPU itself.

namespace eacp::GPU::detail
{
ShaderBackend nativeShaderBackend()
{
    return ShaderBackend::WebGPU;
}
} // namespace eacp::GPU::detail
