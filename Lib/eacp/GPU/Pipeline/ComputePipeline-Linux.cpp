#include "ComputePipeline.h"

#include "../Device/Device.h"
#include "../Shader/ShaderLibrary.h"
#include "../Vulkan/VulkanTypes.h"

// Linux/Vulkan backend. Bakes the library's compute module and a descriptor-set
// layout into a VkPipeline; nativeState() hands the pipeline, both layouts and
// what the kernel declared in the texture range to the pass, which binds them
// together.
//
// The layout is shared where it can be and built here where it cannot. A kernel
// that declares no texture wants exactly the layout every other such kernel
// wants, and VulkanShared holds one; a kernel that declares one needs its own,
// because the binding map gives a texture slot one number whether the kernel
// samples it or writes it while Vulkan gives one binding one descriptor type.
// See VulkanTextureBindings.

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

    // The shared layout when the kernel binds no texture, and one of this
    // pipeline's own when it does. False leaves the pipeline invalid, which is
    // what a caller can act on.
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

    // Deferred for the reason a buffer is: a layout or a pipeline destroyed
    // while a recording that bound it is still open or in flight is a
    // use-after-free the validation layer raises on, and a renderer constructed
    // inside render() does exactly that.
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

    // Whether the layouts in `state` were made for this pipeline and have to go
    // back with it, or are the shared ones every texture-free kernel binds.
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
