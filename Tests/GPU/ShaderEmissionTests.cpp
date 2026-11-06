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
std::string_view spell(const char* text)
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

static_assert(formatDecimal(0) == "0");
static_assert(formatDecimal(-42) == "-42");
static_assert(formatDecimal(2147483647) == "2147483647");

static_assert(printStagedSimdMatrixElement(StagedOperand::Left, "i")
              == "sgmScratch[sgmBase + i]");
static_assert(printStagedSimdMatrixElement(StagedOperand::Right, "i")
              == "sgmScratch[sgmBase + 64u + i]");
static_assert(printSimdMatrixLaneColumn(0) == "sgmColumn");
static_assert(printSimdMatrixLaneColumn(1) == "sgmColumn + 1u");

static_assert(attributeSemantic(Backend::Metal, 3) == " [[attribute(3)]]");
static_assert(attributeSemantic(Backend::DirectX, 3) == " : TEXCOORD3");
static_assert(attributeSemantic(Backend::Vulkan, 3).empty());
static_assert(varyingSemantic(Backend::Metal, 1, ValueType::UInt) == " [[flat]]");
static_assert(varyingSemantic(Backend::Metal, 1, ValueType::Float).empty());
static_assert(flatQualifier(Backend::Vulkan, ValueType::Int) == "flat ");
static_assert(flatQualifier(Backend::DirectX, ValueType::UInt)
              == "nointerpolation ");
static_assert(locationLayout(2) == "layout(location = 2) ");
static_assert(attributeName(Backend::Vulkan, 4) == "attr4");
static_assert(varyingName(Backend::Metal, 4) == "input.v4");
static_assert(indexReference("gid", DispatchRank::TwoD, 1) == "gid.y");
static_assert(indexReference("gid", DispatchRank::OneD, 1) == "gid");
static_assert(bracketed("a + b") == "(a + b)");

static_assert(simdMatrixName(5) == "sgm5");
static_assert(foldedPair(GroupReduction::Max, "a", "b") == "max(a, b)");
static_assert(foldedPair(GroupReduction::Sum, "a", "b") == "a + b");
static_assert(barrierStatement(Backend::Vulkan, "  ")
              == "  memoryBarrierShared();\n  barrier();\n");
static_assert(barrierStatement(Backend::DirectX, "")
              == "GroupMemoryBarrierWithGroupSync();\n");

auto tEmissionBufferTypes = test("ShaderEmission/bufferTypesSpellEachAccess") = []
{
    check(spell(metalBufferType(BufferAccess::Read, ValueType::Float))
          == "device const float*");
    check(spell(metalBufferType(BufferAccess::Write, ValueType::UInt))
          == "device uint*");
    check(spell(metalBufferType(BufferAccess::Atomic, ValueType::Float))
          == "device atomic_uint*");

    check(spell(hlslBufferType(BufferAccess::Read, ValueType::UInt))
          == "StructuredBuffer<uint>");
    check(spell(hlslBufferType(BufferAccess::Write, ValueType::Float))
          == "RWStructuredBuffer<float>");
    check(spell(hlslBufferType(BufferAccess::Atomic, ValueType::Float))
          == "RWStructuredBuffer<uint>");
};

auto tEmissionTextureTypes = test("ShaderEmission/textureTypesSpellEachKind") = []
{
    check(spell(metalTextureType(TextureKind::Texture2D)) == "texture2d<float>");
    check(spell(metalTextureType(TextureKind::Cube)) == "texturecube<float>");
    check(spell(metalTextureType(TextureKind::Depth2DArray))
          == "depth2d_array<float>");

    check(spell(hlslTextureType(TextureKind::Texture2D)) == "Texture2D");
    check(spell(hlslTextureType(TextureKind::Depth2D)) == "Texture2D<float>");
    check(spell(hlslTextureType(TextureKind::Texture2DArray)) == "Texture2DArray");
};

auto tEmissionCallNames = test("ShaderEmission/callNamesRenamePerDialect") = []
{
    check(callName(Backend::Metal, "fract") == "fract");
    check(callName(Backend::DirectX, "fract") == "frac");
    check(callName(Backend::DirectX, "as_type<uint2>") == "asuint");
    check(callName(Backend::Vulkan, "rsqrt") == "inversesqrt");
    check(callName(Backend::Vulkan, "as_type<float4>") == "uintBitsToFloat");
    check(callName(Backend::Vulkan, "float2") == "vec2");
    check(callName(Backend::Vulkan, "log10") == "eacpLog10");
    check(callName(Backend::DirectX, "log10") == "log10");
};

auto tEmissionComparisons = test("ShaderEmission/glslComparisonsAreFunctions") = []
{
    check(spell(glslComparison("<=")) == "lessThanEqual");
    check(spell(glslComparison("!=")) == "notEqual");
    check(glslComparison("&&") == nullptr);
};

auto tEmissionWantsLocal = test("ShaderEmission/onlyOperationsWantALocal") = []
{
    check(wantsLocal(ExprKind::Binary));
    check(wantsLocal(ExprKind::BufferRead));
    check(!wantsLocal(ExprKind::Constant));
    check(!wantsLocal(ExprKind::Swizzle));
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
    check(query.readsStale(loop.value));
    check(!touchesShared(graph, loop));
};

auto tEmissionBoundsGuard = test("ShaderEmission/boundsGuardCoversEveryExtent") = []
{
    check(boundsGuard(DispatchRank::OneD)
          == "    if (gid >= uniforms.count)\n        return;\n");
    check(boundsGuard(DispatchRank::TwoD)
          == "    if (gid.x >= uniforms.width || gid.y >= uniforms.height)\n"
             "        return;\n");
};
