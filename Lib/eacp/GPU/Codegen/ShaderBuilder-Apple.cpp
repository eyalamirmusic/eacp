#include "ShaderBuilder.h"

#include "ShaderEmitter.h"

namespace eacp::GPU::detail
{
ShaderSource nativeShaderSource(const ShaderGraph& graph)
{
    return ShaderSource::msl(emitMetal(graph));
}
} // namespace eacp::GPU::detail
