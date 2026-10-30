#include "ShaderLibrary.h"

#include "../Device/Device.h"
#include "../Spirv/SpirvCompiler.h"
#include "../Vulkan/VulkanTypes.h"
#include "PrecompiledShaders.h"
#include "ShaderSource.h"

#include <cstring>

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

// Copied rather than pointed at: an embedded array has no 4-byte alignment.
Vector<std::uint32_t> spirvWordsFrom(const ResEmbed::DataView& bytes)
{
    auto words = Vector<std::uint32_t>((int) (bytes.size() / sizeof(std::uint32_t)));
    std::memcpy(words.data(), bytes.data(), words.getSize() * sizeof(std::uint32_t));
    return words;
}

// The five-word header with SPIR-V's magic number first, in a whole number of
// words. vkCreateShaderModule is allowed to take anything else on trust.
bool isSpirvModule(const ResEmbed::DataView& bytes)
{
    constexpr auto magic = std::uint32_t {0x07230203};
    constexpr auto headerWords = std::size_t {5};

    if (bytes.size() % sizeof(std::uint32_t) != 0
        || bytes.size() < headerWords * sizeof(std::uint32_t))
        return false;

    auto first = std::uint32_t {};
    std::memcpy(&first, bytes.data(), sizeof(first));
    return first == magic;
}

struct PrecompiledSpirvStage
{
    Spirv::Stage stage;
    VkShaderModule* module;
    int textureBindingBase;
};
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

        if (!loadPrecompiledOrNeedSource(source))
            return;

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

        if (source.hasFragment())
            compileStage(Spirv::Stage::Fragment,
                         source.source,
                         program.fragment,
                         vulkanTextureBinding(0));
    }

    // Deferred, a pipeline holding no reference to the module it was built from.
    ~Native() { releaseModules(); }

    void releaseModules()
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

        program = {};
    }

    Vector<PrecompiledSpirvStage> precompiledStages(const ShaderSource& source)
    {
        if (source.isCompute())
            return {{Spirv::Stage::Compute,
                     &program.compute,
                     vulkanComputeTextureBinding(0)}};

        auto stages = Vector<PrecompiledSpirvStage> {
            {Spirv::Stage::Vertex, &program.vertex, vulkanTextureBinding(0)}};

        if (source.hasFragment())
            stages.add({Spirv::Stage::Fragment,
                        &program.fragment,
                        vulkanTextureBinding(0)});

        return stages;
    }

    // Loads every stage's precompiled SPIR-V, for the target this device was
    // given, when all of them are there, and says whether the source still has
    // to be compiled: not once they have loaded, and not under the require
    // flag, which leaves the library invalid instead.
    bool loadPrecompiledOrNeedSource(const ShaderSource& source)
    {
        const auto key = Precompiled::keyFor(source);
        const auto target = getVulkanShared().getSpirvTarget();
        const auto stages = precompiledStages(source);

        auto modules = Vector<Vector<std::uint32_t>> {};

        for (const auto& stage: stages)
        {
            const auto name =
                Precompiled::spirvResourceName(key, target, stage.stage);
            const auto bytes = Precompiled::find(name);

            if (!bytes)
                return Precompiled::fallBackAfterMiss(name);

            if (!isSpirvModule(bytes))
                return Precompiled::fallBackAfterLoadFailure(
                    name, "it is not a SPIR-V module");

            modules.add(spirvWordsFrom(bytes));
        }

        for (auto index = 0; index < stages.size(); ++index)
        {
            const auto& stage = stages[index];

            if (installStage(
                    modules[index], *stage.module, stage.textureBindingBase))
                continue;

            releaseModules();

            return Precompiled::fallBackAfterLoadFailure(
                Precompiled::spirvResourceName(key, target, stage.stage),
                "vkCreateShaderModule refused it");
        }

        precompiled = true;
        return false;
    }

    void compileStage(Spirv::Stage stage,
                      const std::string& source,
                      VkShaderModule& module,
                      int textureBindingBase)
    {
        const auto result =
            Spirv::compileGlsl(stage, source, getVulkanShared().getSpirvTarget());

        if (!result.log.empty())
            LOG(result.log);

        if (result.succeeded())
            installStage(result.words, module, textureBindingBase);
    }

    bool installStage(const Vector<std::uint32_t>& words,
                      VkShaderModule& module,
                      int textureBindingBase)
    {
        module = makeShaderModule(context->getDevice(), words);

        if (module == VK_NULL_HANDLE)
            return false;

        // The module rather than the graph: the layout has to describe the
        // SPIR-V the driver is given.
        program.textures.merge(spirvTextureBindings(words, textureBindingBase));
        return true;
    }

    VulkanContext* context = nullptr;
    VulkanShaderProgram program;
    bool precompiled = false;
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
           && (!hasFragment() || impl->program.fragment != VK_NULL_HANDLE);
}

bool ShaderLibrary::isPrecompiled() const
{
    return impl->precompiled;
}

void* ShaderLibrary::nativeLibrary() const
{
    return const_cast<VulkanShaderProgram*>(&impl->program);
}
} // namespace eacp::GPU
