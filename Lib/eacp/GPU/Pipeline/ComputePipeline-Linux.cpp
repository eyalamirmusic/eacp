#include "ComputePipeline.h"

#include "../Device/Device.h"
#include "../Shader/ShaderLibrary.h"
#include "../Vulkan/VulkanTypes.h"

namespace eacp::GPU
{
struct ComputePipeline::Native
{
    Native(Device& device, const ShaderLibrary& library)
    {
        if (!device.isValid())
            return;

        context = &getVulkanContext(device);

        auto* program = static_cast<VulkanShaderProgram*>(library.nativeLibrary());

        if (program == nullptr || program->compute == VK_NULL_HANDLE)
            return;

        state.textures = program->textures;

        if (!chooseLayouts())
            return;

        VkPipelineShaderStageCreateInfo stage = {};
        stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
        stage.module = program->compute;
        stage.pName = "main";

        VkComputePipelineCreateInfo info = {};
        info.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
        info.stage = stage;
        info.layout = state.layout;

        if (vkCreateComputePipelines(context->getDevice(),
                                     VK_NULL_HANDLE,
                                     1,
                                     &info,
                                     nullptr,
                                     &state.pipeline)
            != VK_SUCCESS)
        {
            state.pipeline = VK_NULL_HANDLE;
            releaseLayouts();
        }
    }

    bool chooseLayouts()
    {
        if (!state.textures.any())
        {
            const auto& shared = getVulkanShared().getComputeLayouts();

            if (!shared.isValid())
                return false;

            state.layout = shared.pipelineLayout;
            state.setLayout = shared.setLayout;
            return true;
        }

        const auto layouts =
            makeComputeLayouts(context->getDevice(), state.textures);

        if (!layouts.isValid())
            return false;

        state.layout = layouts.pipelineLayout;
        state.setLayout = layouts.setLayout;
        ownsLayouts = true;
        return true;
    }

    // Deferred: a recording still in flight may have bound this layout.
    void releaseLayouts()
    {
        if (!ownsLayouts)
            return;

        ownsLayouts = false;

        context->deferRelease(
            [device = context->getDevice(),
             layout = state.layout,
             setLayout = state.setLayout]
            {
                vkDestroyPipelineLayout(device, layout, nullptr);
                vkDestroyDescriptorSetLayout(device, setLayout, nullptr);
            });

        state.layout = VK_NULL_HANDLE;
        state.setLayout = VK_NULL_HANDLE;
    }

    ~Native()
    {
        if (context == nullptr)
            return;

        if (state.pipeline != VK_NULL_HANDLE)
            context->deferRelease(
                [device = context->getDevice(), pipeline = state.pipeline]
                { vkDestroyPipeline(device, pipeline, nullptr); });

        releaseLayouts();
    }

    VulkanContext* context = nullptr;

    bool ownsLayouts = false;

    VulkanComputePipeline state;
};

ComputePipeline::ComputePipeline(Device& device, const ShaderLibrary& library)
    : impl(device, library)
{
}

bool ComputePipeline::isValid() const
{
    return impl->state.pipeline != VK_NULL_HANDLE;
}

void* ComputePipeline::nativeState() const
{
    return const_cast<VulkanComputePipeline*>(&impl->state);
}
} // namespace eacp::GPU
