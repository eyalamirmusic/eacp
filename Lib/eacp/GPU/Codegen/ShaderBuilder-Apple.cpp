#include "ShaderBuilder.h"

#include "ShaderEmitter.h"

// Apple backend selection: the native shader source is Metal Shading Language.

namespace eacp::GPU::detail
{
ShaderSource nativeShaderSource(const ShaderGraph& graph)
{
    return ShaderSource::msl(emitMetal(graph));
}
} // namespace eacp::GPU::detail
