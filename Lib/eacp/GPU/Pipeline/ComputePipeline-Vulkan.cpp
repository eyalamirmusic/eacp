#include "ComputePipeline.h"

#include "../Device/Device.h"
#include "../Shader/ShaderLibrary.h"
#include "../Vulkan/VulkanContext.h"
#include "../Vulkan/VulkanTypes.h"

namespace eacp::GPU
{
struct ComputePipeline::Native
{
    explicit Native(const ShaderLibrary& library)
    {
        auto& context = getVulkanContext();

        if (!context.isValid() || !library.isValid()
            || library.computeEntry().empty())
            return;

        auto* program = static_cast<VulkanShaderProgram*>(library.nativeLibrary());

        // Shared with every other compute pipeline, for the same reason the
        // render layout is shared: a bound storage set survives a pipeline
        // change instead of being invalidated by it.
        pipeline.layout = context.getComputePipelineLayout();
        pipeline.pushConstantBytes = context.maxUniformBytes();

        auto stage = VkPipelineShaderStageCreateInfo {
            .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
        stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
        stage.module = program->module;
        stage.pName = library.computeEntry().c_str();

        auto info = VkComputePipelineCreateInfo {
            .sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
        info.stage = stage;
        info.layout = pipeline.layout;

        vkCreateComputePipelines(context.getDevice(),
                                 VK_NULL_HANDLE,
                                 1,
                                 &info,
                                 nullptr,
                                 &pipeline.pipeline);
    }

    ~Native()
    {
        if (pipeline.pipeline != VK_NULL_HANDLE)
            getVulkanContext().deferDestroy(pipeline.pipeline);
    }

    VulkanPipeline pipeline;
};

ComputePipeline::ComputePipeline(Device&, const ShaderLibrary& library)
    : impl(library)
{
}

bool ComputePipeline::isValid() const
{
    return impl->pipeline.pipeline != VK_NULL_HANDLE;
}

void* ComputePipeline::nativeState() const
{
    if (!isValid())
        return nullptr;

    return const_cast<VulkanPipeline*>(&impl->pipeline);
}
} // namespace eacp::GPU
