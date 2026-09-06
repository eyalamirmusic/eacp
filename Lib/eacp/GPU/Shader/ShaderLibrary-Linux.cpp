#include "ShaderLibrary.h"

#include "../Device/Device.h"
#include "../Spirv/SpirvCompiler.h"
#include "../Vulkan/VulkanTypes.h"
#include "ShaderSource.h"

// Linux/Vulkan backend. Compiles the GLSL 450 source to SPIR-V with glslang
// (GPU/Spirv) and wraps each stage in a VkShaderModule.
//
// One string, two stages: the GLSL dialect puts a pipeline's vertex and
// fragment halves behind EACP_VERTEX / EACP_FRAGMENT in a single source, so
// each stage is a separate compile of the same text with a different macro
// defined - which is what makes the two impossible to drift apart. The entry
// point is always main; the entry names ShaderSource carries are ignored here
// and kept for API symmetry with the other two backends.
//
// This is the first backend whose shader compiler eacp ships rather than gets
// from the OS. The cost is a fixed ~2 MB per binary and a one-time 90 ms symbol
// table build, which VulkanShared pays at device creation (Spirv::warmUp) so it
// never lands in a frame.

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

        // Both stages reflect against the render binding map and the results
        // are merged: the vertex stage declares no texture and the fragment
        // stage declares them all, and the pipeline binds one set for both.
        compileStage(Spirv::Stage::Vertex,
                     source.source,
                     program.vertex,
                     vulkanTextureBinding(0));
        compileStage(Spirv::Stage::Fragment,
                     source.source,
                     program.fragment,
                     vulkanTextureBinding(0));
    }

    // Deferred rather than destroyed, on the same terms as a buffer: a pipeline
    // holds no reference to the module it was built from, but the pipeline may
    // be created on a recording-adjacent path and the module is cheap to keep
    // one more submission.
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

        // The log is empty on a clean compile, so this forwards it
        // unconditionally and only ever prints when there is something to say.
        if (!result.log.empty())
            LOG(result.log);

        if (!result.succeeded())
            return;

        module = makeShaderModule(context->getDevice(), result.words);

        // The module is reflected rather than the graph that produced it: the
        // pipeline layout has to describe the SPIR-V the driver is given, and a
        // texture binding's descriptor type is a property of that SPIR-V. See
        // VulkanTextureBindings.
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
