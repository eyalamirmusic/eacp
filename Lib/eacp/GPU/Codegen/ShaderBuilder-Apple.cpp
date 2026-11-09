#include "ShaderBuilder.h"

// Apple backend selection: the native shader source is Metal Shading Language.

namespace eacp::GPU::detail
{
ShaderBackend nativeShaderBackend()
{
    return ShaderBackend::Metal;
}
} // namespace eacp::GPU::detail
