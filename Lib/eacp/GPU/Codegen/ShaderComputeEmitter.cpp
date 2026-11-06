#include "ShaderComputeEmitter.h"

#include "../Frame/ComputePass.h"
#include "ShaderBindings.h"
#include "ShaderDeclarations.h"
#include "ShaderGroupSpelling.h"
#include "ShaderHelpers.h"
#include "ShaderStageEmitter.h"

#include <eacp/Core/Utils/Format.h>

#include <cassert>

// Compute kernel emission. The expression printer is the render one; only the
// scaffolding differs: storage buffers and the uniform block are MSL kernel
// parameters but HLSL globals, and the work-item id arrives as a builtin
// parameter on Metal and as SV_DispatchThreadID on D3D. The block always ends
// with the implicit grid extents the bounds guard reads - one count for a 1D
// kernel, a width and a height for a 2D one, a depth as well for a 3D one - and
// the kernel opens with the guard the rounded-up dispatch needs; ComputeProgram
// appends the matching CPU values.
//
// GLSL takes the HLSL shape, with gl_GlobalInvocationID for the id and no stage
// macro: a kernel has one entry point, so its main() is unguarded.

namespace eacp::GPU::ShaderEmission
{
namespace
{
constexpr auto kernelIndent = "    ";

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

// A SIMD-scoped fold takes the same rule with one allowance: a group narrower
// than a SIMD group *is* its own SIMD group, and folding it is well defined.
// What is not is a group that leaves a partial one at its end, where two
// threads a lane apart would fold over different sets.
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

// The kernel's own uniforms, then the implicit grid extents.
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
                              const char* qualifier)
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
                                  const char* qualifier,
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

// Threadgroup memory as the two fallback dialects declare it, globals both,
// followed by the blank line any of it earns.
std::string printGroupMemoryGlobals(const ShaderGraph& graph,
                                    Backend backend,
                                    const char* qualifier)
{
    auto source = printSharedArrays(graph, backend, qualifier);
    source += printReductionScratch(
        graph, backend, qualifier, graph.groupReductionTypes());
    source += formatSimdMatrixScratchDeclaration(graph, qualifier);

    if (doesDeclareGroupMemory(graph))
        source += "\n";

    return source;
}

// The names the entry point declares its indices under, taken out of the
// dialect's three-component builtins.
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

// Textures are kernel parameters like the buffers, on an index space of their
// own. A written one takes the write access qualifier and no sampler: there is
// nothing to sample it with and nothing to read.
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

// What a group reduction folds through: the SIMD-group intrinsics run per SIMD
// group, so combining their partials takes the lane, the SIMD group's index and
// how many of them the threadgroup was given. A kernel holding SIMD-group
// matrices takes the same three, the index being what places the block of the
// output each SIMD group owns.
bool needsMetalSimdBuiltins(const ShaderGraph& graph)
{
    return doesMetalCombinePartials(graph) || graph.usesSimdGroups();
}

std::string printMetalIndexParameters(const ShaderGraph& graph)
{
    auto indexType = getIndexTypeName(graph.dispatchRank());
    auto source = Strings::format("{} gid [[thread_position_in_grid]]", indexType);

    if (graph.usesLocalId())
        source += Strings::format(",\n    {} lid [[thread_position_in_threadgroup]]",
                                  indexType);

    if (graph.usesGroupId())
        source += Strings::format(",\n    {} tgid [[threadgroup_position_in_grid]]",
                                  indexType);

    if (needsMetalSimdBuiltins(graph))
        source += ",\n    uint simdLane [[thread_index_in_simdgroup]],\n    "
                  "uint simdIndex [[simdgroup_index_in_threadgroup]],\n    "
                  "uint simdCount [[simdgroups_per_threadgroup]]";

    return source;
}

// Threadgroup arrays are body-scope declarations on Metal, ahead of everything
// that subscripts them. Only the wide folds stage anything, and only where the
// group is more than one SIMD group: everything else is the intrinsic alone.
std::string printMetalGroupMemory(const ShaderGraph& graph)
{
    constexpr auto qualifier = "    threadgroup";
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

// One descriptor set carries the lot, at the Metal indices.
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

// The three builtins are uvec3 whatever the rank, so each index is the swizzle
// of its own, exactly as the HLSL semantics are.
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
           + printKernelIndices(graph,
                                {getGlslIndexTypeName(graph.dispatchRank()),
                                 "gl_GlobalInvocationID",
                                 "gl_LocalInvocationID",
                                 "gl_WorkGroupID"});
}

// SRV t<slot> / UAV u<slot> with one shared slot counter, matching the flat
// Metal indices ComputePass binds both backends with.
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

// Textures are globals here, and their registers start above every buffer
// slot's: a texture and a storage buffer share the t and u spaces on this
// backend, while their slots are counted separately. See
// ComputePass::textureRegisterBase.
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

// The flattened local index the scratch tree walks, which cs_5_0 hands over as
// a semantic of its own rather than leaving it to be derived. A kernel holding
// SIMD-group matrices needs it too: with no wave matrix operation to lower to,
// it is what stands in for the SIMD group's index and what picks the lane that
// makes a fragment's store.
bool needsHlslGroupLane(const ShaderGraph& graph)
{
    return graph.usesGroupReduction() || graph.usesSimdGroups();
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

    if (needsHlslGroupLane(graph))
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
           + printKernelIndices(graph,
                                {getIndexTypeName(graph.dispatchRank()),
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

// Where the fallback keeps a fragment is fixed by the lane, so the two backends
// that take it work that out once, ahead of the body.
//
// The early-return bounds guard the rounded-up dispatch needs - except in a
// kernel that barriers, where a return some threads take ahead of a barrier the
// rest sit at is undefined on both backends. There every thread runs the whole
// body, and the kernel bounds its own stores against
// gridCount()/gridWidth()/gridHeight()/gridDepth() instead.
std::string printKernelPrologue(const ShaderGraph& graph, Backend backend)
{
    auto source = std::string {};

    if (backend != Backend::Metal && graph.simdMatrixCount() > 0)
        source += formatSimdMatrixPreamble(backend);

    if (!graph.usesBarrier())
        source += formatBoundsGuard(graph.dispatchRank());

    return source;
}

// Stores ride the statement stream like everything else, so the body is one
// block walk: a write records where it was made, inside whatever loop or branch
// was open, and the emitter has no end-of-kernel step.
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
