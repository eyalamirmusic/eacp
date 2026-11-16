#include "ComputePipeline.h"

#include "../Device/Device.h"
#include "../Shader/ShaderLibrary.h"
#include "../WebGPU/WebGPUContext.h"

#include <eacp/Core/Utils/Logging.h>

#include <memory>
#include <string>
#include <vector>

namespace eacp::GPU
{
struct ComputePipeline::Native
{
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

        state.variantFor = [this](const std::vector<WebGPUStorageFormat>& formats)
        { return variantFor(formats); };
    }

    bool build(WebGPUComputePipeline& target,
               const wgpu::ShaderModule& module,
               const std::vector<WebGPUBinding>& bindings)
    {
        target.layout = context.makeBindingLayout(bindings, true);

        const auto overrides = WebGPUConstantEntries {constants};

        auto descriptor = wgpu::ComputePipelineDescriptor {};
        descriptor.layout = target.layout.pipeline;
        descriptor.compute.module = module;
        descriptor.compute.entryPoint =
            wgpu::StringView {entry.data(), entry.size()};
        descriptor.compute.constantCount = overrides.entries.size();
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

    // The kernel recompiled with the storage formats bound, once per set.
    const WebGPUComputePipeline*
        variantFor(const std::vector<WebGPUStorageFormat>& formats)
    {
        for (const auto& [key, variant]: variants)
            if (key == formats)
                return variant.get();

        auto variant = std::make_unique<WebGPUComputePipeline>();
        const auto source = withStorageFormats(program.source, formats);

        if (!build(*variant,
                   makeWgslModule(context.getDevice(), source),
                   scanWgslBindings(source)))
            variant->pipeline = nullptr;

        variants.emplace_back(formats, std::move(variant));
        return variants.back().second.get();
    }

    WebGPUContext& context;
    PipelineConstants constants;
    std::string entry;
    WebGPUShaderProgram program;
    WebGPUComputePipeline state;
    std::vector<std::pair<std::vector<WebGPUStorageFormat>,
                          std::unique_ptr<WebGPUComputePipeline>>>
        variants;
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
