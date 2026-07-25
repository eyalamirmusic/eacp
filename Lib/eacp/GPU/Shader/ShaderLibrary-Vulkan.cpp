#include "ShaderLibrary.h"

#include "../Device/Device.h"
#include "../Vulkan/VulkanContext.h"
#include "../Vulkan/VulkanTypes.h"
#include "ShaderSource.h"

// Vulkan backend. A ShaderSource carries SPIR-V words rather than text, so there
// is no compile step here at all -- the module is handed to the driver as-is and
// the pipeline picks its stages out by entry-point name. Both stages live in one
// module, which is what the emitter produces.

namespace eacp::GPU
{
struct ShaderLibrary::Native
{
    explicit Native(const ShaderSource& source)
    {
        auto& context = getVulkanContext();

        if (!context.isValid() || source.words.size() == 0)
            return;

        auto info =
            VkShaderModuleCreateInfo {VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
        info.codeSize =
            static_cast<std::size_t>(source.words.size()) * sizeof(std::uint32_t);
        info.pCode = &source.words[0];

        vkCreateShaderModule(context.getDevice(), &info, nullptr, &program.module);
    }

    ~Native()
    {
        auto& context = getVulkanContext();

        if (context.isValid() && program.module != VK_NULL_HANDLE)
            vkDestroyShaderModule(context.getDevice(), program.module, nullptr);
    }

    VulkanShaderProgram program;
};

ShaderLibrary::ShaderLibrary(Device&, const ShaderSource& source)
    : vertexEntryName(source.vertexEntry)
    , fragmentEntryName(source.fragmentEntry)
    , computeEntryName(source.computeEntry)
    , impl(source)
{
}

bool ShaderLibrary::isValid() const
{
    return impl->program.module != VK_NULL_HANDLE;
}

void* ShaderLibrary::nativeLibrary() const
{
    return const_cast<VulkanShaderProgram*>(&impl->program);
}
} // namespace eacp::GPU
