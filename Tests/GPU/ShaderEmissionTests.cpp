#include "CodegenCommon.h"

#include <eacp/GPU/Codegen/ShaderDeclarations.h>
#include <eacp/GPU/Codegen/ShaderDialect.h>
#include <eacp/GPU/Codegen/ShaderGraphAnalysis.h>
#include <eacp/GPU/Codegen/ShaderGroupSpelling.h>

// The pieces the emitter is built from, each checked on its own: the type
// spellings the dialects share a table for, the builtin renames, and the graph
// walks a stage is planned from.

using namespace nano;
using namespace eacp;
using namespace eacp::GPU;
using namespace eacp::GPU::ShaderEmission;

namespace ShaderEmissionTests
{
std::string_view spell(std::string_view text)
{
    return text;
}

struct CountingKernel
{
    CountingKernel()
    {
        auto output = builder.outputBuffer();
        auto counter = builder.var(0u);

        builder.loop(counter < loopCount, [&] { counter += 1u; });
        builder.write(output, builder.threadId(), toFloat(counter.get()));
    }

    static constexpr auto loopCount = 4u;
    ShaderBuilder builder;
};

int findStatement(const ShaderGraph& graph, StatementKind kind)
{
    for (auto index: graph.block(ShaderGraph::rootBlock).statements)
        if (graph.statement(index).kind == kind)
            return index;

    return -1;
}
} // namespace ShaderEmissionTests

using namespace ShaderEmissionTests;

static_assert(getInterpolationTypeQualifier(
                  Backend::Vulkan, defaultInterpolation(ValueType::Int).type)
              == "flat ");
static_assert(getInterpolationTypeQualifier(
                  Backend::DirectX, defaultInterpolation(ValueType::UInt).type)
              == "nointerpolation ");
static_assert(getPositionSemantic(Backend::Metal) == " [[position]]");
static_assert(getComponentSuffix(2) == ".z");
static_assert(getIndexSwizzle(DispatchRank::TwoD) == ".xy");
static_assert(getMetalSimdReduction(GroupReduction::Min) == "simd_min");

auto tEmissionSpellings = test("ShaderEmission/spellingsMatchTheEmittedSource") = []
{
    check(printStagedSimdMatrixElement(StagedOperand::Left, "i")
          == "sgmScratch[sgmBase + i]");
    check(printStagedSimdMatrixElement(StagedOperand::Right, "i")
          == "sgmScratch[sgmBase + 64u + i]");
    check(printSimdMatrixLaneColumn(0) == "sgmColumn");
    check(printSimdMatrixLaneColumn(1) == "sgmColumn + 1u");
    check(getAttributeSemantic(Backend::Metal, 3) == " [[attribute(3)]]");
    check(getAttributeSemantic(Backend::DirectX, 3) == " : TEXCOORD3");
    check(getAttributeSemantic(Backend::Vulkan, 3).empty());
    check(
        getVaryingSemantic(Backend::Metal, 1, defaultInterpolation(ValueType::UInt))
        == " [[flat]]");
    check(
        getVaryingSemantic(Backend::Metal, 1, defaultInterpolation(ValueType::Float))
            .empty());
    check(getLocationLayout(2) == "layout(location = 2) ");
    check(getAttributeName(Backend::Vulkan, 4) == "attr4");
    check(getVaryingName(Backend::Metal, 4) == "input.v4");
    check(getIndexReference("gid", DispatchRank::TwoD, 1) == "gid.y");
    check(getIndexReference("gid", DispatchRank::OneD, 1) == "gid");
    check(wrapInParentheses("a + b") == "(a + b)");
    check(getSimdMatrixName(5) == "sgm5");
    check(formatFoldedPair(GroupReduction::Max, "a", "b") == "max(a, b)");
    check(formatFoldedPair(GroupReduction::Sum, "a", "b") == "a + b");
    check(formatBarrierStatement(Backend::Vulkan, "  ")
          == "  memoryBarrierShared();\n  barrier();\n");
    check(formatBarrierStatement(Backend::DirectX, "")
          == "GroupMemoryBarrierWithGroupSync();\n");
};

auto tEmissionBufferTypes = test("ShaderEmission/bufferTypesSpellEachAccess") = []
{
    check(spell(getMetalBufferType(BufferAccess::Read, ValueType::Float))
          == "device const float*");
    check(spell(getMetalBufferType(BufferAccess::Write, ValueType::UInt))
          == "device uint*");
    check(spell(getMetalBufferType(BufferAccess::Atomic, ValueType::Float))
          == "device atomic_uint*");

    check(spell(getHlslBufferType(BufferAccess::Read, ValueType::UInt))
          == "StructuredBuffer<uint>");
    check(spell(getHlslBufferType(BufferAccess::Write, ValueType::Float))
          == "RWStructuredBuffer<float>");
    check(spell(getHlslBufferType(BufferAccess::Atomic, ValueType::Float))
          == "RWStructuredBuffer<uint>");
};

auto tEmissionTextureTypes = test("ShaderEmission/textureTypesSpellEachKind") = []
{
    check(spell(getMetalTextureType(TextureKind::Texture2D)) == "texture2d<float>");
    check(spell(getMetalTextureType(TextureKind::Cube)) == "texturecube<float>");
    check(spell(getMetalTextureType(TextureKind::Depth2DArray))
          == "depth2d_array<float>");

    check(spell(getHlslTextureType(TextureKind::Texture2D)) == "Texture2D");
    check(spell(getHlslTextureType(TextureKind::Depth2D)) == "Texture2D<float>");
    check(spell(getHlslTextureType(TextureKind::Texture2DArray))
          == "Texture2DArray");
};

auto tEmissionCallNames = test("ShaderEmission/callNamesRenamePerDialect") = []
{
    check(getCallName(Backend::Metal, "fract") == "fract");
    check(getCallName(Backend::DirectX, "fract") == "frac");
    check(getCallName(Backend::DirectX, "as_type<uint2>") == "asuint");
    check(getCallName(Backend::Vulkan, "rsqrt") == "inversesqrt");
    check(getCallName(Backend::Vulkan, "as_type<float4>") == "uintBitsToFloat");
    check(getCallName(Backend::Vulkan, "float2") == "vec2");
    check(getCallName(Backend::Vulkan, "log10") == "eacpLog10");
    check(getCallName(Backend::DirectX, "log10") == "log10");
};

auto tEmissionComparisons = test("ShaderEmission/glslComparisonsAreFunctions") = []
{
    check(spell(getGlslComparisonFunction("<=")) == "lessThanEqual");
    check(spell(getGlslComparisonFunction("!=")) == "notEqual");
    check(getGlslComparisonFunction("&&").empty());
};

auto tEmissionWantsLocal = test("ShaderEmission/onlyOperationsWantALocal") = []
{
    check(isLocalWanted(ExprKind::Binary));
    check(isLocalWanted(ExprKind::BufferRead));
    check(!isLocalWanted(ExprKind::Constant));
    check(!isLocalWanted(ExprKind::Swizzle));
};

auto tEmissionLoopConditions =
    test("ShaderEmission/loopConditionsCountOnlyWhenIncluded") = []
{
    auto kernel = CountingKernel {};
    const auto& graph = kernel.builder.graph();

    auto included = Vector<int> {};
    auto excluded = Vector<int> {};
    collectStatementExpressions(
        graph, ShaderGraph::rootBlock, LoopConditions::Included, included);
    collectStatementExpressions(
        graph, ShaderGraph::rootBlock, LoopConditions::Excluded, excluded);

    check(included.size() == excluded.size() + 1);
};

auto tEmissionStaleness =
    test("ShaderEmission/aWrittenVariableMakesItsReadStale") = []
{
    auto kernel = CountingKernel {};
    const auto& graph = kernel.builder.graph();
    const auto& loop = graph.statement(findStatement(graph, StatementKind::Loop));

    auto written = Vector<char> {};
    written.resize(graph.variables().size(), 0);
    collectWrites(graph, loop, written);

    auto buffersWritten = Vector<char> {};
    buffersWritten.resize(graph.storageBuffers().size(), 0);

    auto seen = VisitSet {graph.nodeCount()};
    auto query = StalenessQuery {graph, written, buffersWritten, false, seen};

    check(written.contains(1));
    check(query.doesReadStaleValue(loop.value));
    check(!doesTouchSharedMemory(graph, loop));
};

auto tEmissionBoundsGuard = test("ShaderEmission/boundsGuardCoversEveryExtent") = []
{
    check(formatBoundsGuard(DispatchRank::OneD)
          == "    if (gid >= uniforms.count)\n        return;\n");
    check(formatBoundsGuard(DispatchRank::TwoD)
          == "    if (gid.x >= uniforms.width || gid.y >= uniforms.height)\n"
             "        return;\n");
};
