#include "ShaderLibrary.h"

#include "../Device/Device.h"
#include "../Spirv/SpirvCompiler.h"
#include "../Vulkan/VulkanTypes.h"
#include "ShaderBinaryCache.h"
#include "ShaderSource.h"

#include <cstring>
#include <string>

// The entry names ShaderSource carries are ignored: the entry point is main.
//
// glslang is the slow half of building a pipeline and the driver's half is
// already kept in the VkPipelineCache, so the SPIR-V it produces is kept on disk
// too (ShaderBinaryCache) and a later launch reads the words back instead of
// compiling the same source again.

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

Vector<std::uint32_t> wordsOf(const std::string& bytes)
{
    auto words = Vector<std::uint32_t> {};
    words.resize((int) (bytes.size() / sizeof(std::uint32_t)));
    std::memcpy(words.data(), bytes.data(), bytes.size());
    return words;
}

// SPIR-V for one stage of the source, from the disk cache where a previous
// launch compiled it, and compiled and stored there otherwise.
Vector<std::uint32_t> spirvFor(Spirv::Stage stage, const std::string& source)
{
    const auto compiler = Spirv::compilerIdentity();
    const auto key = std::to_string((int) stage) + '\n' + source;

    if (auto cached = ShaderBinaryCache::load(compiler, key);
        cached.has_value() && !cached->empty()
        && cached->size() % sizeof(std::uint32_t) == 0)
        return wordsOf(*cached);

    auto result = Spirv::compileGlsl(stage, source);

    if (!result.log.empty())
        LOG(result.log);

    if (result.succeeded())
        ShaderBinaryCache::store(
            compiler,
            key,
            std::string_view {reinterpret_cast<const char*>(result.words.data()),
                              result.words.getSize() * sizeof(std::uint32_t)});

    return std::move(result.words);
}
} // namespace

struct ShaderLibrary::Native
{
    Native(Device& device, const ShaderSource& source)
    {
        // An empty source is a build something declined to make, and whatever
        // declined it has already said why - see ComputeProgram::prepare. There
        // is nothing here to compile and nothing for glslang to report.
        if (!device.isValid() || source.source.empty())
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
        const auto words = spirvFor(stage, source);

        if (words.empty())
            return;

        module = makeShaderModule(context->getDevice(), words);

        // The module rather than the graph: the layout has to describe the
        // SPIR-V the driver is given.
        if (module != VK_NULL_HANDLE)
            program.textures.merge(spirvTextureBindings(words, textureBindingBase));
    }

    VulkanContext* context = nullptr;
    VulkanShaderProgram program;
};

ShaderLibrary::ShaderLibrary(Device& device, const ShaderSource& source)
    : vertexEntryName(source.vertexEntry)
    , fragmentEntryName(source.fragmentEntry)
    , computeEntryName(source.computeEntry)
    , groupShape(source.threadGroup)
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
