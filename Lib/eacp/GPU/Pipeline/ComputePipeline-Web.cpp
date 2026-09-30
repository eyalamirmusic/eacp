#include "ComputePipeline.h"

#include "../Device/Device.h"
#include "../Shader/ShaderLibrary.h"
#include "../WebGPU/WebGPUTypes.h"

namespace eacp::GPU
{
struct ComputePipeline::Native
{
    Native(Device& device, const ShaderLibrary& library)
    {
        if (!device.isValid())
            return;

        auto* program = static_cast<WebShaderProgram*>(library.nativeLibrary());

        if (program == nullptr || !program->isValid())
            return;

        const auto shape = library.threadGroupShape();
        const auto& limits = getWebGPUShared().getLimits();

        if (shape.isSet()
            && (std::uint32_t) shape.threadCount()
                   > limits.maxComputeInvocationsPerWorkgroup)
        {
            LOG("WebGPU: a kernel of ",
                shape.threadCount(),
                " threads a group; this device allows ",
                limits.maxComputeInvocationsPerWorkgroup);
            return;
        }

        state.bindings = program->bindings;
        state.groupLayout = program->groupLayout;
        wgpuBindGroupLayoutAddRef(state.groupLayout);

        auto descriptor = WGPU_COMPUTE_PIPELINE_DESCRIPTOR_INIT;
        descriptor.label = toWebString("eacp compute pipeline");
        descriptor.layout = program->pipelineLayout;
        descriptor.compute.module = program->module;
        descriptor.compute.entryPoint = toWebString(library.computeEntry());

        pushWebErrorScope();

        state.pipeline = wgpuDeviceCreateComputePipeline(
            getWebGPUShared().getDevice(), &descriptor);

        popWebErrorScope("a compute pipeline (" + library.computeEntry() + ")");
    }

    ~Native()
    {
        if (state.pipeline != nullptr)
            wgpuComputePipelineRelease(state.pipeline);

        if (state.groupLayout != nullptr)
            wgpuBindGroupLayoutRelease(state.groupLayout);
    }

    WebComputePipeline state;
};

ComputePipeline::ComputePipeline(Device& device, const ShaderLibrary& library)
    : groupShape(library.threadGroupShape())
    , impl(device, library)
{
}

bool ComputePipeline::isValid() const
{
    return impl->state.isValid();
}

// WGSL has no SIMD-group width to report; subgroups are an optional feature.
int ComputePipeline::threadExecutionWidth() const
{
    return 0;
}

void* ComputePipeline::nativeState() const
{
    return const_cast<WebComputePipeline*>(&impl->state);
}
} // namespace eacp::GPU
