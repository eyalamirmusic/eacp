#include "ShaderComputeEmitter.h"

#include "../Frame/ComputePass.h"
#include "ShaderBindings.h"
#include "ShaderDeclarations.h"
#include "ShaderFunctionEmission.h"
#include "ShaderGroupSpelling.h"
#include "ShaderHelpers.h"
#include "ShaderStageEmitter.h"
#include "ShaderWgslHelpers.h"

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

[[maybe_unused]] bool hasValidSimdGroupShape(const ShaderGraph& graph)
{
    return !graph.usesSimdGroups() || isWholeNumberOfSimdGroups(graph);
}

// A group narrower than one SIMD group is its own SIMD group; only a partial
// trailing one makes a fold undefined.
[[maybe_unused]] bool hasValidSimdReductionShape(const ShaderGraph& graph)
{
    return !graph.usesSimdReduction() || isNarrowerThanOneSimdGroup(graph)
           || isWholeNumberOfSimdGroups(graph);
}

[[maybe_unused]] bool takesNoDerivedLevel(const ShaderGraph& graph)
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

[[maybe_unused]] bool readsOnlyKernelBuiltIns(const ShaderGraph& graph)
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

// HLSL and GLSL act atomically on plain integers; Metal and WGSL need their
// atomic types.
std::string_view getSharedElementType(const SharedArray& shared, Backend backend)
{
    auto isSigned = shared.elementType == ValueType::Int;

    if (shared.atomic && backend == Backend::Metal)
        return isSigned ? "atomic_int" : "atomic_uint";

    if (shared.atomic && backend == Backend::WebGPU)
        return isSigned ? "atomic<i32>" : "atomic<u32>";

    return getDialectTypeName(backend, shared.elementType);
}

// A group memory array: `threadgroup float s0[64];` in the C family,
// `var<workgroup> s0: array<f32, 64>;` in WGSL.
std::string printGroupArray(Backend backend,
                            std::string_view qualifier,
                            std::string_view type,
                            const std::string& name,
                            int elements)
{
    if (backend == Backend::WebGPU)
        return Strings::format(
            "{} {}: array<{}, {}>;\n", qualifier, name, type, elements);

    return Strings::format("{} {} {}[{}];\n", qualifier, type, name, elements);
}

std::string printSharedArrays(const ShaderGraph& graph,
                              Backend backend,
                              std::string_view qualifier)
{
    auto source = std::string {};

    for (auto i = 0; i < graph.sharedArrays().size(); ++i)
    {
        const auto& shared = graph.sharedArrays()[i];

        source += printGroupArray(backend,
                                  qualifier,
                                  getSharedElementType(shared, backend),
                                  Strings::format("s{}", i),
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
        source += printGroupArray(backend,
                                  qualifier,
                                  getDialectTypeName(backend, elementType),
                                  std::string(getGroupScratchName(elementType)),
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
        source += Strings::format("{} buffer{} [[buffer({})]],\n    ",
                                  getMetalSlotBufferType(graph, buffers[i], i),
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
        source += formatGlslSlotBufferBlock(
            graph, buffers[i], i, vulkanComputeBufferBinding(i));

    if (buffers.size() > 0)
        source += "\n";

    for (auto i = 0; i < graph.textureCount(); ++i)
        source += formatGlslTexture(graph, i, vulkanComputeTextureBinding(i));

    if (graph.textureCount() > 0)
        source += "\n";

    return source;
}

std::string printGlslKernelOpening(const ShaderGraph& graph,
                                   const std::string& functions)
{
    const auto group = graph.threadGroupShape();

    auto source = printGlslKernelBindings(graph);
    source += printGroupMemoryGlobals(graph, Backend::Vulkan, "shared");
    source += Strings::format(
        "layout(local_size_x = {}, local_size_y = {}, local_size_z = {}) in;\n\n",
        group.x,
        group.y,
        group.z);
    source += functions;
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

        source += Strings::format("{} buffer{} : register({}{});\n",
                                  getHlslSlotBufferType(graph, buffers[i], i),
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

std::string printHlslKernelOpening(const ShaderGraph& graph,
                                   const std::string& functions)
{
    auto source = printHlslKernelBuffers(graph);
    source += printHlslKernelTextures(graph);
    source += printGroupMemoryGlobals(graph, Backend::DirectX, "groupshared");
    source += functions;
    source += printHlslKernelSignature(graph);

    return source
           + printKernelIndices(
               graph,
               {getIndexTypeName(Backend::DirectX, graph.dispatchRank()),
                "threadId",
                "localThread",
                "groupIndex"});
}

std::string printWgslKernelBindings(const ShaderGraph& graph)
{
    const auto& buffers = graph.storageBuffers();
    auto source = std::string {};

    for (auto i = 0; i < buffers.size(); ++i)
        source += formatWgslBuffer(graph, buffers[i], i);

    if (buffers.size() > 0)
        source += "\n";

    for (auto i = 0; i < graph.textureCount(); ++i)
        source += formatWgslTexture(graph, i);

    if (graph.textureCount() > 0)
        source += "\n";

    return source;
}

// The builtins arrive as uint vectors of three, taken to the dispatch's rank
// under the names the other dialects give them.
std::string printWgslKernelSignature(const ShaderGraph& graph)
{
    const auto group = graph.threadGroupShape();

    auto source =
        Strings::format("@compute @workgroup_size({}, {}, {})\nfn computeMain("
                        "@builtin(global_invocation_id) gidIn: vec3<u32>",
                        group.x,
                        group.y,
                        group.z);

    if (graph.usesLocalId())
        source += ",\n    @builtin(local_invocation_id) lidIn: vec3<u32>";

    if (graph.usesGroupId())
        source += ",\n    @builtin(workgroup_id) tgidIn: vec3<u32>";

    if (doesHlslReadGroupLane(graph))
        source += ",\n    @builtin(local_invocation_index) groupLane: u32";

    if (graph.dispatchRank() == DispatchRank::OneD)
        source += ",\n    @builtin(num_workgroups) gridGroups: vec3<u32>";
    else if (graph.usesBuiltIn(ShaderBuiltIn::GroupCount))
        source += ",\n    @builtin(num_workgroups) groupCount: vec3<u32>";

    return source + ")\n{\n";
}

// WebGPU dispatches at most 65535 groups along an axis, so a 1D dispatch past
// that arrives as rows of groups, and its thread and group indices are taken
// linear over them; the bounds guard drops the threads past the count.
std::string printWgslLinearIndices(const ShaderGraph& graph)
{
    auto source = Strings::format(
        "    let gid: u32 = gidIn.x + gidIn.y * (gridGroups.x * {}u);\n",
        graph.threadGroupShape().x);

    if (graph.usesLocalId())
        source += "    let lid: u32 = lidIn.x;\n";

    if (graph.usesGroupId())
        source += "    let tgid: u32 = tgidIn.x + tgidIn.y * gridGroups.x;\n";

    return source;
}

std::string printWgslKernelIndices(const ShaderGraph& graph)
{
    if (graph.dispatchRank() == DispatchRank::OneD)
        return printWgslLinearIndices(graph);

    const auto type = getIndexTypeName(Backend::WebGPU, graph.dispatchRank());
    const auto swizzle = getIndexSwizzle(graph.dispatchRank());

    auto declare = [&](std::string_view name)
    {
        return Strings::format(
            "    let {}: {} = {}In{};\n", name, type, name, swizzle);
    };

    auto source = declare("gid");

    if (graph.usesLocalId())
        source += declare("lid");

    if (graph.usesGroupId())
        source += declare("tgid");

    return source;
}

std::string printWgslKernelOpening(const ShaderGraph& graph,
                                   const std::string& functions)
{
    auto source = printWgslKernelBindings(graph);
    source +=
        printGroupMemoryGlobals(graph, Backend::WebGPU, wgslWorkgroupQualifier);
    source += functions;
    source += printWgslKernelSignature(graph);
    return source + printWgslKernelIndices(graph);
}

// The functions the kernel calls are defined after every global they may name
// and ahead of the entry point.
std::string printKernelOpening(const ShaderGraph& graph,
                               Backend backend,
                               const std::string& functions)
{
    if (backend == Backend::Metal)
        return functions + printMetalKernelOpening(graph);

    if (backend == Backend::Vulkan)
        return printGlslKernelOpening(graph, functions);

    if (backend == Backend::WebGPU)
        return printWgslKernelOpening(graph, functions);

    return printHlslKernelOpening(graph, functions);
}

Vector<int> getKernelRoots(const ShaderGraph& graph)
{
    auto roots = Vector<int> {};
    collectStatementExpressions(
        graph, ShaderGraph::rootBlock, LoopConditions::Included, roots);
    return roots;
}

// WGSL checks a function against the stage calling it: a kernel's take no
// derivative.
[[maybe_unused]] bool calledFunctionsTakeNoDerivedLevel(const ShaderGraph& graph,
                                                        const Vector<int>& functions)
{
    auto roots = Vector<int> {};

    for (auto function: functions)
        collectStatementExpressions(
            graph, graph.function(function).body, LoopConditions::Included, roots);

    auto seen = VisitSet {graph.nodeCount()};
    auto pending = roots;

    while (!pending.empty())
    {
        auto node = pending.back();
        pending.pop_back();

        if (node < 0 || !seen.visit(node))
            continue;

        const auto& expr = graph.expr(node);

        if (takesDerivedLevel(graph, expr))
            return false;

        for (auto argument: expr.args)
            pending.add(argument);
    }

    return true;
}

// No early return in a kernel that barriers: threads returning ahead of a
// barrier the rest wait at is undefined, so such a kernel bounds its own stores.
std::string printKernelPrologue(const ShaderGraph& graph, Backend backend)
{
    auto source = std::string {};

    if (backend != Backend::Metal && graph.simdMatrixCount() > 0)
        source += formatSimdMatrixPreamble(backend);

    if (!graph.usesBarrier())
        source += formatBoundsGuard(graph.dispatchRank(), backend);

    return source;
}

std::string printKernelBody(const ShaderGraph& graph, Backend backend)
{
    auto stageRoots = getKernelRoots(graph);
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

    const auto functions = findCalledFunctions(graph, getKernelRoots(graph));

    assert(calledFunctionsTakeNoDerivedLevel(graph, functions)
           && "eacp: a kernel called a function that takes the fragment's "
              "derivatives - dfdx, dfdy, fwidth, or a sample with no level. A "
              "kernel has none of them.");

    assert((backend != Backend::WebGPU || usesOnlyWgslIdentifiers(graph))
           && "eacp: a struct, a member, a function or an override is named with "
              "a word WGSL reserves, or with a double underscore, which no WGSL "
              "identifier may be.");

    auto source = getSourcePreamble(graph, backend);
    source += printKernelUniformBlock(graph, backend);
    source += printKernelOpening(
        graph, backend, printFunctionDefinitions(graph, backend, functions));
    source += printKernelPrologue(graph, backend);
    source += printKernelBody(graph, backend);
    source += "}\n";
    return source;
}
} // namespace eacp::GPU::ShaderEmission
