#include "Samplers.h"

#include <cassert>

namespace eacp::GPU::detail
{
int addSampledFunction(ShaderGraph* graph,
                       int textureSlot,
                       const SamplerHandle& sampler,
                       TextureFunction function,
                       const Vector<int>& args,
                       int component)
{
    assert(graph != nullptr && sampler.graph == graph
           && "eacp: a sampler samples the textures of the shader declaring it");

    return graph->addTextureFunction(
        textureSlot, function, args, component, sampler.slot);
}
} // namespace eacp::GPU::detail
