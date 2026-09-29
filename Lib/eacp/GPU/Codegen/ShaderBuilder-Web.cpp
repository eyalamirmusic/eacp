#include "ShaderBuilder.h"

#include "ShaderEmitter.h"

// Web backend selection: the native shader source is WGSL, and each sampled
// texture's sampling rides beside it for the sampler bindings the backend makes.

namespace eacp::GPU::detail
{
ShaderSource nativeShaderSource(const ShaderGraph& graph)
{
    auto source = ShaderSource::wgsl(emitWgsl(graph));
    auto stage = graph.isCompute() ? ShaderStage::Compute : ShaderStage::Fragment;

    for (auto slot = 0; slot < graph.textureCount(); ++slot)
    {
        if (graph.textureAccess(slot) == TextureAccess::Write)
            continue;

        auto binding = ResourceBinding {};
        binding.kind = ResourceKind::Sampler;
        binding.stage = stage;
        binding.index = slot;
        binding.name = "sampler" + std::to_string(slot);
        binding.sampling = graph.textureSampling(slot);
        source.withBinding(std::move(binding));
    }

    return source;
}
} // namespace eacp::GPU::detail
