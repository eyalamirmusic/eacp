#include "ShaderComputeEmitter.h"

#include "../Frame/ComputePass.h"
#include "ShaderBindings.h"
#include "ShaderDeclarations.h"
#include "ShaderGroupSpelling.h"
#include "ShaderHelpers.h"
#include "ShaderStageEmitter.h"

#include <eacp/Core/Utils/Format.h>

#include <cassert>

namespace eacp::GPU::ShaderEmission
{
namespace
{
constexpr auto kernelIndent = std::string_view {"    "};

std::string_view getIndexTypeName(Backend backend, DispatchRank rank)
{
    constexpr auto indexTypes =
        Array<ValueType, 3> {ValueType::UInt, ValueType::UInt2, ValueType::UInt3};
    return getDialectTypeName(backend, indexTypes[getGridExtentCount(rank) - 1]);
}

bool isWholeNumberOfSimdGroups(const ShaderGraph& graph)
{
    return graph.threadGroupShape().threadCount() % simdGroupWidth == 0;
}

bool isNarrowerThanOneSimdGroup(const ShaderGraph& graph)
{
    return graph.threadGroupShape().threadCount() <= simdGroupWidth;
}

bool hasValidSimdGroupShape(const ShaderGraph& graph)
{
    return !graph.usesSimdGroups() || isWholeNumberOfSimdGroups(graph);
}

// A group narrower than one SIMD group is its own SIMD group; only a partial
// trailing one makes a fold undefined.
bool hasValidSimdReductionShape(const ShaderGraph& graph)
{
    return !graph.usesSimdReduction() || isNarrowerThanOneSimdGroup(graph)
           || isWholeNumberOfSimdGroups(graph);
}

void assertKernelShapeIsValid(const ShaderGraph& graph)
{
    assert(hasValidSimdGroupShape(graph)
           && "eacp: a kernel using SIMD groups has to be dispatched in a "
              "threadgroup of a whole number of them - a multiple of "
              "ComputeProgram::simdWidth threads. A group that is not leaves a "
              "partial SIMD group, whose matrix operations are undefined.");

    assert(hasValidSimdReductionShape(graph)
           && "eacp: a kernel using simdSum/simdMax/simdMin has to be "
              "dispatched in a threadgroup that is a whole number of SIMD "
              "groups - a multiple of ComputeProgram::simdWidth threads - or "
              "in one narrower than a single SIMD group.");

    assert(graph.storageBuffers().size() <= ComputePass::portableBufferSlots
           && "eacp: a kernel may bind ComputePass::portableBufferSlots storage "
              "buffers. A slot past that has no register the root signature "
              "declares, so it binds nowhere and reads zeroes.");

    (void) graph;
}

std::string printKernelUniformBlock(const ShaderGraph& graph, Backend backend)
{
    auto rank = graph.dispatchRank();
    auto uniformTypes = graph.uniforms();
    auto uniformNames = Vector<std::string> {};

    for (auto i = 0; i < uniformTypes.size(); ++i)
        uniformNames.add(Strings::format("u{}", i));

    if (rank == DispatchRank::OneD)
    {
        uniformTypes.add(ValueType::UInt);
        uniformNames.add("count");
    }
    else
    {
        for (auto i = 0; i < getGridExtentCount(rank); ++i)
        {
            uniformTypes.add(ValueType::UInt);
            uniformNames.add(std::string(getGridExtentName(i)));
        }
    }

    return formatUniformBlock(
        backend, uniformTypes, uniformNames, vulkanComputeUniformBinding);
}

std::string printSharedArrays(const ShaderGraph& graph,
                              Backend backend,
                              std::string_view qualifier)
{
    auto source = std::string {};

    for (auto i = 0; i < graph.sharedArrays().size(); ++i)
    {
        const auto& shared = graph.sharedArrays()[i];

        source += Strings::format("{} {} s{}[{}];\n",
                                  qualifier,
                                  getDialectTypeName(backend, shared.elementType),
                                  i,
                                  shared.elements);
    }

    return source;
}

std::string printReductionScratch(const ShaderGraph& graph,
                                  Backend backend,
                                  std::string_view qualifier,
                                  const Vector<ValueType>& elementTypes)
{
    auto source = std::string {};

    for (auto elementType: elementTypes)
        source += Strings::format("{} {} {}[{}];\n",
                                  qualifier,
                                  getDialectTypeName(backend, elementType),
                                  getGroupScratchName(elementType),
                                  getThreadsPerGroup(graph));

    return source;
}

std::string printGroupMemoryGlobals(const ShaderGraph& graph,
                                    Backend backend,
                                    std::string_view qualifier)
{
    auto source = printSharedArrays(graph, backend, qualifier);
    source += printReductionScratch(
        graph, backend, qualifier, graph.groupReductionTypes());
    source += formatSimdMatrixScratchDeclaration(graph, qualifier);

    if (hasGroupMemoryDeclarations(graph))
        source += "\n";

    return source;
}

struct KernelIndexSources
{
    std::string_view typeName;
    std::string_view global;
    std::string_view local;
    std::string_view group;
};

std::string printKernelIndices(const ShaderGraph& graph,
                               const KernelIndexSources& sources)
{
    auto swizzle = getIndexSwizzle(graph.dispatchRank());
    auto source = Strings::format(
        "    {} gid = {}{};\n", sources.typeName, sources.global, swizzle);

    if (graph.usesLocalId())
        source += Strings::format(
            "    {} lid = {}{};\n", sources.typeName, sources.local, swizzle);

    if (graph.usesGroupId())
        source += Strings::format(
            "    {} tgid = {}{};\n", sources.typeName, sources.group, swizzle);

    return source;
}

std::string printMetalBufferParameters(const ShaderGraph& graph)
{
    const auto& buffers = graph.storageBuffers();
    auto source = std::string {};

    for (auto i = 0; i < buffers.size(); ++i)
        source += Strings::format(
            "{} buffer{} [[buffer({})]],\n    ",
            getMetalBufferType(buffers[i], graph.storageElementType(i)),
            i,
            i);

    return source;
}

std::string printMetalTextureParameters(const ShaderGraph& graph)
{
    auto source = std::string {};

    for (auto i = 0; i < graph.textureCount(); ++i)
    {
        if (graph.textureAccess(i) == TextureAccess::Write)
        {
            source += Strings::format(
                "texture2d<float, access::write> texture{} [[texture({})]],\n    ",
                i,
                i);
            continue;
        }

        source += Strings::format("{} texture{} [[texture({})]],\n    sampler "
                                  "sampler{} [[sampler({})]],\n    ",
                                  getMetalTextureType(graph.textureKind(i)),
                                  i,
                                  i,
                                  i,
                                  i);
    }

    return source;
}

std::string printMetalIndexParameters(const ShaderGraph& graph)
{
    auto indexType = getIndexTypeName(Backend::Metal, graph.dispatchRank());
    auto source = Strings::format("{} gid [[thread_position_in_grid]]", indexType);

    if (graph.usesLocalId())
        source += Strings::format(",\n    {} lid [[thread_position_in_threadgroup]]",
                                  indexType);

    if (graph.usesGroupId())
        source += Strings::format(",\n    {} tgid [[threadgroup_position_in_grid]]",
                                  indexType);

    if (doesMetalCombinePartials(graph) || graph.usesSimdGroups())
        source += ",\n    uint simdLane [[thread_index_in_simdgroup]],\n    "
                  "uint simdIndex [[simdgroup_index_in_threadgroup]],\n    "
                  "uint simdCount [[simdgroups_per_threadgroup]]";

    return source;
}

std::string printMetalGroupMemory(const ShaderGraph& graph)
{
    constexpr auto qualifier = std::string_view {"    threadgroup"};
    auto source = printSharedArrays(graph, Backend::Metal, qualifier);

    if (doesMetalCombinePartials(graph))
        source += printReductionScratch(
            graph, Backend::Metal, qualifier, graph.wholeGroupReductionTypes());

    return source;
}

std::string printMetalKernelOpening(const ShaderGraph& graph)
{
    auto source = std::string {"kernel void computeMain("};
    source += printMetalBufferParameters(graph);
    source += printMetalTextureParameters(graph);
    source += Strings::format("constant Uniforms& uniforms [[buffer({})]],\n    ",
                              ComputePass::uniformBase);
    source += printMetalIndexParameters(graph);
    source += ")\n{\n";
    return source + printMetalGroupMemory(graph);
}

std::string printGlslKernelBindings(const ShaderGraph& graph)
{
    const auto& buffers = graph.storageBuffers();
    auto source = std::string {};

    for (auto i = 0; i < buffers.size(); ++i)
        source += formatGlslBufferBlock(buffers[i],
                                        graph.storageElementType(i),
                                        i,
                                        vulkanComputeBufferBinding(i));

    if (buffers.size() > 0)
        source += "\n";

    for (auto i = 0; i < graph.textureCount(); ++i)
    {
        auto binding = vulkanComputeTextureBinding(i);

        if (graph.textureAccess(i) == TextureAccess::Write)
            source += formatGlslWritableTexture(i, binding);
        else
            source += formatGlslSampledTexture(graph, i, binding);
    }

    if (graph.textureCount() > 0)
        source += "\n";

    return source;
}

std::string printGlslKernelOpening(const ShaderGraph& graph)
{
    const auto group = graph.threadGroupShape();

    auto source = printGlslKernelBindings(graph);
    source += printGroupMemoryGlobals(graph, Backend::Vulkan, "shared");
    source += Strings::format(
        "layout(local_size_x = {}, local_size_y = {}, local_size_z = {}) in;\n\n",
        group.x,
        group.y,
        group.z);
    source += "void main()\n{\n";

    return source
           + printKernelIndices(
               graph,
               {getIndexTypeName(Backend::Vulkan, graph.dispatchRank()),
                "gl_GlobalInvocationID",
                "gl_LocalInvocationID",
                "gl_WorkGroupID"});
}

std::string printHlslKernelBuffers(const ShaderGraph& graph)
{
    const auto& buffers = graph.storageBuffers();
    auto source = std::string {};

    for (auto i = 0; i < buffers.size(); ++i)
    {
        auto registerSpace = buffers[i] == BufferAccess::Read ? 't' : 'u';

        source += Strings::format(
            "{} buffer{} : register({}{});\n",
            getHlslBufferType(buffers[i], graph.storageElementType(i)),
            i,
            registerSpace,
            i);
    }

    if (buffers.size() > 0)
        source += "\n";

    return source;
}

// Texture registers start above every buffer slot: both share t and u here.
std::string printHlslKernelTextures(const ShaderGraph& graph)
{
    auto source = std::string {};

    for (auto i = 0; i < graph.textureCount(); ++i)
    {
        auto textureRegister = ComputePass::textureRegisterBase + i;

        if (graph.textureAccess(i) == TextureAccess::Write)
        {
            source +=
                Strings::format("RWTexture2D<float4> texture{} : register(u{});\n",
                                i,
                                textureRegister);
            continue;
        }

        source += Strings::format("{} texture{} : register(t{});\n",
                                  getHlslTextureType(graph.textureKind(i)),
                                  i,
                                  textureRegister);
    }

    source += formatHlslSamplerDeclarations(graph);

    if (graph.textureCount() > 0)
        source += "\n";

    return source;
}

std::string printHlslKernelSignature(const ShaderGraph& graph)
{
    const auto group = graph.threadGroupShape();

    auto source =
        Strings::format("[numthreads({}, {}, {})]\n", group.x, group.y, group.z);
    source += "void computeMain(uint3 threadId : SV_DispatchThreadID";

    if (graph.usesLocalId())
        source += ", uint3 localThread : SV_GroupThreadID";

    if (graph.usesGroupId())
        source += ", uint3 groupIndex : SV_GroupID";

    if (graph.usesGroupReduction() || graph.usesSimdGroups())
        source += ", uint groupLane : SV_GroupIndex";

    return source + ")\n{\n";
}

std::string printHlslKernelOpening(const ShaderGraph& graph)
{
    auto source = printHlslKernelBuffers(graph);
    source += printHlslKernelTextures(graph);
    source += printGroupMemoryGlobals(graph, Backend::DirectX, "groupshared");
    source += printHlslKernelSignature(graph);

    return source
           + printKernelIndices(
               graph,
               {getIndexTypeName(Backend::DirectX, graph.dispatchRank()),
                "threadId",
                "localThread",
                "groupIndex"});
}

std::string printKernelOpening(const ShaderGraph& graph, Backend backend)
{
    if (backend == Backend::Metal)
        return printMetalKernelOpening(graph);

    if (backend == Backend::Vulkan)
        return printGlslKernelOpening(graph);

    return printHlslKernelOpening(graph);
}

// No early return in a kernel that barriers: threads returning ahead of a
// barrier the rest wait at is undefined, so such a kernel bounds its own stores.
std::string printKernelPrologue(const ShaderGraph& graph, Backend backend)
{
    auto source = std::string {};

    if (backend != Backend::Metal && graph.simdMatrixCount() > 0)
        source += formatSimdMatrixPreamble(backend);

    if (!graph.usesBarrier())
        source += formatBoundsGuard(graph.dispatchRank());

    return source;
}

std::string printKernelBody(const ShaderGraph& graph, Backend backend)
{
    auto stageRoots = Vector<int> {};
    collectStatementExpressions(
        graph, ShaderGraph::rootBlock, LoopConditions::Included, stageRoots);

    auto stage = StageEmitter {graph, backend};

    auto source = stage.declareArrays(stageRoots, kernelIndent);
    source += stage.emitBlock(ShaderGraph::rootBlock, kernelIndent);
    return source;
}
} // namespace

std::string emitCompute(const ShaderGraph& graph, Backend backend)
{
    assertKernelShapeIsValid(graph);

    auto source = getSourcePreamble(graph, backend);
    source += printKernelUniformBlock(graph, backend);
    source += printKernelOpening(graph, backend);
    source += printKernelPrologue(graph, backend);
    source += printKernelBody(graph, backend);
    source += "}\n";
    return source;
}
} // namespace eacp::GPU::ShaderEmission
