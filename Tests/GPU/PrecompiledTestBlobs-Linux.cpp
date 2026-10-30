#include "PrecompiledTestBlobs.h"

#include <eacp/GPU/Shader/PrecompiledNames.h>
#include <eacp/GPU/Spirv/SpirvCompiler.h>

#include <NanoTest/NanoTest.h>

// The Vulkan backend's tests, desktop Linux and Android alike: eacp-gpu is not
// built there without eacp-spirv, so the compiler is always at hand.
namespace eacp::GPU::PrecompiledTestBlobs
{
namespace
{
constexpr Spirv::Target allSpirvTargets[] = {Spirv::Target::vulkan11Spirv13,
                                             Spirv::Target::vulkan11Spirv14,
                                             Spirv::Target::vulkan13Spirv16};

Vector<Spirv::Stage> spirvStagesOf(const ShaderSource& source)
{
    if (source.isCompute())
        return {Spirv::Stage::Compute};

    auto stages = Vector<Spirv::Stage> {Spirv::Stage::Vertex};

    if (source.hasFragment())
        stages.add(Spirv::Stage::Fragment);

    return stages;
}
} // namespace

// Every target, since which one the device was given is the backend's choice;
// only that one is looked up.
Vector<std::string> resourceNamesFor(const ShaderSource& source)
{
    const auto key = Precompiled::keyFor(source);
    auto names = Vector<std::string> {};

    for (auto target: allSpirvTargets)
        for (auto stage: spirvStagesOf(source))
            names.add(Precompiled::spirvResourceName(key, target, stage));

    return names;
}

Resources compileAheadOfTime(const ShaderSource& source)
{
    const auto key = Precompiled::keyFor(source);
    auto resources = Resources {};

    for (auto target: allSpirvTargets)
    {
        for (auto stage: spirvStagesOf(source))
        {
            const auto result = Spirv::compileGlsl(stage, source.source, target);

            nano::check(result.succeeded(), result.log);

            if (!result.succeeded())
                return {};

            resources.add({Precompiled::spirvResourceName(key, target, stage),
                           bytesOf(result.words.data(),
                                   result.words.getSize() * sizeof(std::uint32_t))});
        }
    }

    return resources;
}
} // namespace eacp::GPU::PrecompiledTestBlobs
