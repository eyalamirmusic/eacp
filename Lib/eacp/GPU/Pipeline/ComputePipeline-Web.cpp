#include "ComputePipeline.h"

#include "../Device/Device.h"
#include "../Shader/ShaderLibrary.h"
#include "../WebGPU/WebGPUContext.h"

#include <eacp/Core/Utils/Logging.h>

#include <string>

namespace eacp::GPU
{
struct ComputePipeline::Native
{
    // The kernel rebuilt for one set of bound storage formats and unfilterable
    // textures.
    struct Variant
    {
        WebGPUComputeVariantKey key;
        OwningPointer<WebGPUComputePipeline> pipeline;
    };

    Native(Device& device,
           const ShaderLibrary& library,
           const PipelineConstants& constantsToUse)
        : context(getWebGPUContext(device))
        , constants(constantsToUse)
        , entry(library.computeEntry())
    {
        auto* compiled = static_cast<WebGPUShaderProgram*>(library.nativeLibrary());

        if (!context.isValid() || compiled == nullptr || compiled->module == nullptr
            || entry.empty())
            return;

        program = *compiled;
        build(state, program.module, program.bindings);

        state.variantFor = [this](const WebGPUComputeVariantKey& key)
        { return variantFor(key); };
    }

    bool build(WebGPUComputePipeline& target,
               const wgpu::ShaderModule& module,
               const Vector<WebGPUBinding>& bindings)
    {
        target.layout = context.makeBindingLayout(bindings, true);

        const auto overrides = WebGPUConstantEntries {constants};

        auto descriptor = wgpu::ComputePipelineDescriptor {};
        descriptor.layout = target.layout.pipeline;
        descriptor.compute.module = module;
        descriptor.compute.entryPoint =
            wgpu::StringView {entry.data(), entry.size()};
        descriptor.compute.constantCount = overrides.entries.getSize();
        descriptor.compute.constants = overrides.entries.data();

        auto message = std::string {};
        auto pipeline = wgpu::ComputePipeline {};

        const auto built = context.succeeds(
            [&]
            { pipeline = context.getDevice().CreateComputePipeline(&descriptor); },
            &message);

        if (!built)
        {
            LOG("WebGPU: a compute pipeline was refused: ", message);
            return false;
        }

        target.pipeline = pipeline;
        return true;
    }

    // The kernel recompiled with the storage formats bound, and laid out with
    // the unfilterable textures bound, once per set.
    const WebGPUComputePipeline* variantFor(const WebGPUComputeVariantKey& key)
    {
        for (const auto& variant: variants)
            if (variant.key == key)
                return variant.pipeline.get();

        auto* variant = variants.create(Variant {key, {}}).pipeline.create();
        const auto rewritten = !key.storageFormats.empty();
        const auto source =
            rewritten ? withStorageFormats(program.source, key.storageFormats)
                      : program.source;
        const auto module =
            rewritten ? makeWgslModule(context.getDevice(), source) : program.module;
        const auto bindings = withUnfilterableTextures(
            scanWgslBindings(source), key.unfilterable, source);

        if (!build(*variant, module, bindings))
            variant->pipeline = nullptr;

        return variant;
    }

    WebGPUContext& context;
    PipelineConstants constants;
    std::string entry;
    WebGPUShaderProgram program;
    WebGPUComputePipeline state;
    Vector<Variant> variants;
};

ComputePipeline::ComputePipeline(Device& device, const ShaderLibrary& library)
    : ComputePipeline(device, library, PipelineConstants {})
{
}

ComputePipeline::ComputePipeline(Device& device,
                                 const ShaderLibrary& library,
                                 const PipelineConstants& constants)
    : groupShape(library.threadGroupShape())
    , impl(device, library, constants)
{
}

bool ComputePipeline::isValid() const
{
    return impl->state.pipeline != nullptr;
}

// The EDSL emulates a SIMD group at its own width here, as on Vulkan, so
// there is no hardware number to report.
int ComputePipeline::threadExecutionWidth() const
{
    return 0;
}

void* ComputePipeline::nativeState() const
{
    return const_cast<WebGPUComputePipeline*>(&impl->state);
}
} // namespace eacp::GPU
