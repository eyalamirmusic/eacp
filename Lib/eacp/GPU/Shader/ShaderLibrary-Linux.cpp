#include "ShaderLibrary.h"

#include "../Device/Device.h"
#include "../Spirv/SpirvCompiler.h"
#include "../Vulkan/VulkanTypes.h"
#include "ShaderSource.h"

// The entry names ShaderSource carries are ignored: the entry point is main.

namespace eacp::GPU
{
namespace
{
VkShaderModule makeShaderModule(VkDevice device, const Vector<std::uint32_t>& words)
{
    VkShaderModuleCreateInfo info = {};
    info.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    info.codeSize = words.getSize() * sizeof(std::uint32_t);
    info.pCode = words.data();

    auto module = VkShaderModule {VK_NULL_HANDLE};

    if (vkCreateShaderModule(device, &info, nullptr, &module) != VK_SUCCESS)
        return VK_NULL_HANDLE;

    return module;
}
} // namespace

struct ShaderLibrary::Native
{
    Native(Device& device, const ShaderSource& source)
    {
        if (!device.isValid())
            return;

        context = &getVulkanContext(device);

        if (source.isCompute())
        {
            compileStage(Spirv::Stage::Compute,
                         source.source,
                         program.compute,
                         vulkanComputeTextureBinding(0));
            return;
        }

        // Two compiles of one source, guarded by EACP_VERTEX and EACP_FRAGMENT.
        compileStage(Spirv::Stage::Vertex,
                     source.source,
                     program.vertex,
                     vulkanTextureBinding(0));
        compileStage(Spirv::Stage::Fragment,
                     source.source,
                     program.fragment,
                     vulkanTextureBinding(0));
    }

    // Deferred, a pipeline holding no reference to the module it was built from.
    ~Native()
    {
        if (context == nullptr)
            return;

        for (auto module: {program.vertex, program.fragment, program.compute})
        {
            if (module == VK_NULL_HANDLE)
                continue;

            context->deferRelease(
                [device = context->getDevice(), module]
                { vkDestroyShaderModule(device, module, nullptr); });
        }
    }

    void compileStage(Spirv::Stage stage,
                      const std::string& source,
                      VkShaderModule& module,
                      int textureBindingBase)
    {
        const auto result = Spirv::compileGlsl(stage, source);

        if (!result.log.empty())
            LOG(result.log);

        if (!result.succeeded())
            return;

        module = makeShaderModule(context->getDevice(), result.words);

        // The module rather than the graph: the layout has to describe the
        // SPIR-V the driver is given.
        if (module != VK_NULL_HANDLE)
            program.textures.merge(
                spirvTextureBindings(result.words, textureBindingBase));
    }

    VulkanContext* context = nullptr;
    VulkanShaderProgram program;
};

ShaderLibrary::ShaderLibrary(Device& device, const ShaderSource& source)
    : vertexEntryName(source.vertexEntry)
    , fragmentEntryName(source.fragmentEntry)
    , computeEntryName(source.computeEntry)
    , impl(device, source)
{
}

bool ShaderLibrary::isValid() const
{
    if (impl->program.compute != VK_NULL_HANDLE)
        return true;

    return impl->program.vertex != VK_NULL_HANDLE
           && impl->program.fragment != VK_NULL_HANDLE;
}

void* ShaderLibrary::nativeLibrary() const
{
    return const_cast<VulkanShaderProgram*>(&impl->program);
}
} // namespace eacp::GPU
