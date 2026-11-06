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

bool takesNoDerivedLevel(const ShaderGraph& graph)
{
    for (auto node = 0; node < graph.nodeCount(); ++node)
    {
        const auto& expr = graph.expr(node);

        if (expr.kind == ExprKind::TextureFunction
            && usesImplicitDerivatives(textureFunctionNamed(expr.text)))
            return false;
    }

    return true;
}

bool readsOnlyKernelBuiltIns(const ShaderGraph& graph)
{
    for (auto node = 0; node < graph.nodeCount(); ++node)
    {
        const auto& expr = graph.expr(node);

        if (expr.kind == ExprKind::BuiltIn
            && stageOf(static_cast<ShaderBuiltIn>(expr.index))
                   != ShaderStage::Compute)
            return false;
    }

    return true;
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

    assert(readsOnlyKernelBuiltIns(graph)
           && "eacp: a kernel read a render stage's built-in - "
              "fragmentCoordinate, vertexId, frontFacing and the like belong "
              "to a ShaderProgram, and a kernel has none of them.");

    assert(takesNoDerivedLevel(graph)
           && "eacp: a kernel used sample, sampleBias or sampleCompare on a "
              "kind from TextureFunctions.h. They take the level from the "
              "fragment's derivatives, which a kernel has none of - use "
              "sampleLevel, sampleGrad or sampleCompareLevel.");

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

// HLSL and GLSL act atomically on plain integers; Metal needs its atomic types.
std::string_view getSharedElementType(const SharedArray& shared, Backend backend)
{
    if (shared.atomic && backend == Backend::Metal)
        return shared.elementType == ValueType::Int ? "atomic_int" : "atomic_uint";

    return getDialectTypeName(backend, shared.elementType);
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
                                  getSharedElementType(shared, backend),
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
        if (graph.textureAccess(i) == TextureAccess::Sampler)
        {
            source +=
                Strings::format("sampler sampler{} [[sampler({})]],\n    ", i, i);
            continue;
        }

        source += Strings::format("{} texture{} [[texture({})]],\n    ",
                                  getMetalTextureParameterType(graph, i),
                                  i,
                                  i);

        if (graph.textureAccess(i) == TextureAccess::Sample)
            source +=
                Strings::format("sampler sampler{} [[sampler({})]],\n    ", i, i);
    }

    return source;
}

std::string printMetalBuiltInParameters(const ShaderGraph& graph)
{
    auto source = std::string {};

    if (graph.usesBuiltIn(ShaderBuiltIn::LocalIndex))
        source += ",\n    uint localIndex [[thread_index_in_threadgroup]]";

    if (graph.usesBuiltIn(ShaderBuiltIn::GroupCount))
        source +=
            Strings::format(",\n    {} groupCount [[threadgroups_per_grid]]",
                            getIndexTypeName(Backend::Metal, graph.dispatchRank()));

    if (graph.usesBuiltIn(ShaderBuiltIn::SimdGroupSize))
        source += ",\n    uint simdSize [[threads_per_simdgroup]]";

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

    auto usesSimdVocabulary =
        doesMetalCombinePartials(graph) || graph.usesSimdGroups();

    if (usesSimdVocabulary || graph.usesBuiltIn(ShaderBuiltIn::SimdLaneIndex))
        source += ",\n    uint simdLane [[thread_index_in_simdgroup]]";

    if (usesSimdVocabulary)
        source += ",\n    uint simdIndex [[simdgroup_index_in_threadgroup]],\n    "
                  "uint simdCount [[simdgroups_per_threadgroup]]";

    return source + printMetalBuiltInParameters(graph);
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
        source += formatGlslTexture(graph, i, vulkanComputeTextureBinding(i));

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
        if (graph.textureAccess(i) == TextureAccess::Sampler)
            source += formatHlslSampler(graph, i);
        else
            source += Strings::format("{} texture{} : register({}{});\n",
                                      getHlslTextureDeclarationType(graph, i),
                                      i,
                                      getHlslTextureRegisterSpace(graph, i),
                                      ComputePass::textureRegisterBase + i);
    }

    source += formatHlslSamplerDeclarations(graph);

    if (graph.textureCount() > 0)
        source += "\n";

    return source;
}

bool doesHlslReadGroupLane(const ShaderGraph& graph)
{
    return graph.usesGroupReduction() || graph.usesSimdGroups()
           || graph.usesBuiltIn(ShaderBuiltIn::LocalIndex)
           || graph.usesBuiltIn(ShaderBuiltIn::SimdLaneIndex);
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

    if (doesHlslReadGroupLane(graph))
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

    assert(!graph.hasStatementOf(StatementKind::Discard)
           && "eacp: discard belongs to the fragment stage; a kernel ends an "
              "invocation with returnEarly.");

    auto source = getSourcePreamble(graph, backend);
    source += printKernelUniformBlock(graph, backend);
    source += printKernelOpening(graph, backend);
    source += printKernelPrologue(graph, backend);
    source += printKernelBody(graph, backend);
    source += "}\n";
    return source;
}
} // namespace eacp::GPU::ShaderEmission
