#include "ComputePipeline.h"

#include "../Device/Device.h"
#include "../Shader/ShaderLibrary.h"
#include "../Vulkan/VulkanTypes.h"

// Linux/Vulkan backend. Bakes the library's compute module and the shared
// compute pipeline layout into a VkPipeline; nativeState() hands the pipeline
// and both layouts to the pass, which binds them together.

namespace eacp::GPU
{
struct ComputePipeline::Native
{
    Native(Device& device, const ShaderLibrary& library)
    {
        if (!device.isValid())
            return;

        context = &getVulkanContext(device);

        const auto& layouts = getVulkanShared().getComputeLayouts();

        if (!layouts.isValid())
            return;

        auto* program = static_cast<VulkanShaderProgram*>(library.nativeLibrary());

        if (program == nullptr || program->compute == VK_NULL_HANDLE)
            return;

        // A kernel that declares a texture is refused rather than built. The
        // binding map gives a texture slot the same binding whether it is
        // sampled or written, and Vulkan gives a binding one descriptor type, so
        // the shared layout has to pick - and there is nothing to bind into
        // either kind until stage 3's Texture exists. Building the pipeline
        // anyway would mean a dispatch reading a descriptor nothing ever wrote,
        // which is undefined behaviour rather than a wrong picture. isValid()
        // false is what a caller can act on; see spirvBindsTextureRange.
        if (program->usesTextures)
        {
            LOG("Vulkan: a kernel that binds a texture cannot run yet - the "
                "Linux Texture backend arrives with the render half");
            return;
        }

        VkPipelineShaderStageCreateInfo stage = {};
        stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
        stage.module = program->compute;
        stage.pName = "main";

        VkComputePipelineCreateInfo info = {};
        info.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
        info.stage = stage;
        info.layout = layouts.pipelineLayout;

        if (vkCreateComputePipelines(context->getDevice(),
                                     VK_NULL_HANDLE,
                                     1,
                                     &info,
                                     nullptr,
                                     &state.pipeline)
            != VK_SUCCESS)
        {
            state.pipeline = VK_NULL_HANDLE;
            return;
        }

        state.layout = layouts.pipelineLayout;
        state.setLayout = layouts.setLayout;
    }

    // Deferred for the reason a buffer is: a pipeline destroyed while a
    // recording that bound it is still open or in flight is a use-after-free
    // the validation layer raises on, and a renderer constructed inside render()
    // does exactly that.
    ~Native()
    {
        if (context == nullptr || state.pipeline == VK_NULL_HANDLE)
            return;

        context->deferRelease(
            [device = context->getDevice(), pipeline = state.pipeline]
            { vkDestroyPipeline(device, pipeline, nullptr); });
    }

    VulkanContext* context = nullptr;
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
