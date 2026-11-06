#include "ShaderGroupSpelling.h"

#include <eacp/Core/Utils/Format.h>

namespace eacp::GPU::ShaderEmission
{

// How many threads one group holds, whatever its rank - what a group reduction
// folds over and what its scratch is sized for.
int getThreadsPerGroup(const ShaderGraph& graph)
{
    return graph.threadGroupShape().threadCount();
}

// The first halving step of a tree over that many threads: the largest power of
// two below the count, so a count that is not one still folds its tail in first.
int getReductionStride(int threads)
{
    auto stride = 1;

    while (stride * 2 < threads)
        stride *= 2;

    return stride;
}

// How many threads one fold spans: the whole group, or one SIMD group of it -
// and a group smaller than a SIMD group is its own SIMD group, so the narrow
// scope never reaches past what was dispatched.
int getReductionWidth(const ShaderGraph& graph, ReductionScope scope)
{
    auto threads = getThreadsPerGroup(graph);

    if (scope == ReductionScope::Group)
        return threads;

    return threads < simdGroupWidth ? threads : simdGroupWidth;
}

// Whether a fold spans the whole group either because that is its scope or
// because the group is one SIMD group and the two are the same set of threads.
bool doesFoldSpanWholeGroup(const ShaderGraph& graph, ReductionScope scope)
{
    return getReductionWidth(graph, scope) == getThreadsPerGroup(graph);
}

// Whether Metal answers a fold with the SIMD-group intrinsic alone, which is
// the narrow scope and only the narrow scope.
//
// Not a whole-group fold in a group of simdGroupWidth threads or fewer, however
// much it looks like the same thing: that argument holds only where the
// hardware SIMD group is at least as wide as the group, and the hardware's
// width is a property of the compiled pipeline (threadExecutionWidth) rather
// than a constant. An Intel Mac dispatches a kernel at eight or sixteen lanes,
// so a 32-thread group there is two or four SIMD groups and simd_sum alone
// would fold a quarter of it. The wide fold keeps the combine at every width -
// simdCount is whatever the hardware gave - which is what it was always for.
bool doesMetalFoldInOneInstruction(ReductionScope scope)
{
    return scope == ReductionScope::Simd;
}

// So a kernel whose folds are all SIMD-scoped on Metal declares neither the
// scratch nor the three SIMD-group builtins the combine walks.
bool doesMetalCombinePartials(const ShaderGraph& graph)
{
    return !graph.wholeGroupReductionTypes().empty();
}

// Where a fragment's patch lives, spelled as the emitted source names it.
std::string getSimdMatrixMemoryName(const Statement& statement)
{
    auto prefix = statement.memory == SimdMatrixMemory::Shared ? "s" : "buffer";
    return Strings::format("{}{}", prefix, statement.bufferSlot);
}

// How the fallback backends hold a fragment: spread over the lanes of what
// would have been the SIMD group the way Metal spreads it, each lane owning
// the pair of elements at row lane / 4, columns (lane % 4) * 2 and the next -
// which is elements 2 * lane and 2 * lane + 1 of the row-major patch. A pair
// rather than the whole 8x8, because FXC counts every per-thread array against
// one budget of 4096 registers for the kernel, and a blocked product holding a
// few dozen fragments as 64 floats each went over it.
constexpr int simdMatrixLaneElements =
    simdMatrixSize * simdMatrixSize / simdGroupWidth;
constexpr int simdMatrixLanesPerRow = simdMatrixSize / simdMatrixLaneElements;

static_assert(simdMatrixLaneElements == 2,
              "the fallback holds a lane's share of a fragment as a two-vector");

std::string_view getSimdMatrixLaneType(Backend backend)
{
    return getDialectTypeName(backend, ValueType::Float2);
}

// The scratch a product stages its two operands in, whole, so each lane can
// read the row and the columns other lanes hold: two fragments per SIMD group,
// one slice per SIMD group of the threadgroup.
constexpr int simdMatrixScratchPerGroup = 2 * simdMatrixSize * simdMatrixSize;

int getSimdMatrixScratchElementCount(const ShaderGraph& graph)
{
    return graph.threadGroupShape().threadCount() / simdGroupWidth
           * simdMatrixScratchPerGroup;
}

// The scratch declared, under the storage qualifier the dialect gives
// threadgroup memory, in a kernel that holds any fragment at all.
std::string formatSimdMatrixScratchDeclaration(const ShaderGraph& graph,
                                               const std::string& qualifier)
{
    if (graph.simdMatrixCount() == 0)
        return {};

    return Strings::format("{} float {}[{}];\n",
                           qualifier,
                           simdMatrixScratchName,
                           getSimdMatrixScratchElementCount(graph));
}

bool hasGroupMemoryDeclarations(const ShaderGraph& graph)
{
    return graph.sharedArrays().size() > 0 || graph.usesGroupReduction()
           || graph.simdMatrixCount() > 0;
}

// What the fallback addresses a fragment by, declared once at the top of any
// kernel that holds one: the lane within its notional SIMD group, the row and
// the first column of the pair that lane holds, and where that group's slice
// of the scratch starts.
std::string formatSimdMatrixPreamble(Backend backend)
{
    auto lane = getGroupLaneName(backend);

    auto source =
        Strings::format("    uint sgmLane = {} % {}u;\n", lane, simdGroupWidth);
    source +=
        Strings::format("    uint sgmRow = sgmLane / {}u;\n", simdMatrixLanesPerRow);
    source += Strings::format("    uint sgmColumn = (sgmLane % {}u) * {}u;\n",
                              simdMatrixLanesPerRow,
                              simdMatrixLaneElements);
    source += Strings::format("    uint sgmBase = ({} / {}u) * {}u;\n",
                              lane,
                              simdGroupWidth,
                              simdMatrixScratchPerGroup);
    return source;
}

// Two partials combined, spelled identically in all three dialects.
std::string formatFoldedPair(GroupReduction operation,
                             const std::string& left,
                             const std::string& right)
{
    switch (operation)
    {
        case GroupReduction::Sum:
            return Strings::format("{} + {}", left, right);
        case GroupReduction::Max:
            return Strings::format("max({}, {})", left, right);
        case GroupReduction::Min:
            return Strings::format("min({}, {})", left, right);
    }

    return Strings::format("{} + {}", left, right);
}

// The barrier itself, shared by the barrier statement and by the reductions
// that bracket their scratch with one.
std::string formatBarrierStatement(Backend backend, const std::string& indent)
{
    if (backend == Backend::Vulkan)
        return Strings::format(
            "{}memoryBarrierShared();\n{}barrier();\n", indent, indent);

    auto barrier = backend == Backend::Metal
                       ? "threadgroup_barrier(mem_flags::mem_threadgroup);\n"
                       : "GroupMemoryBarrierWithGroupSync();\n";
    return indent + barrier;
}

// A numbering of its own, so it collides with neither the variables nor the
// shared arrays.
std::string getSimdMatrixName(int slot)
{
    return Strings::format("sgm{}", slot);
}

// An element of an operand a product staged in the scratch: the left fragment
// at the start of this SIMD group's slice, the right one a fragment further on.
std::string printStagedSimdMatrixElement(StagedOperand operand,
                                         std::string_view element)
{
    if (operand == StagedOperand::Right)
        return Strings::format("{}[sgmBase + {}u + {}]",
                               simdMatrixScratchName,
                               simdMatrixSize * simdMatrixSize,
                               element);

    return Strings::format("{}[sgmBase + {}]", simdMatrixScratchName, element);
}

// The first or second of the pair of columns a lane holds.
std::string printSimdMatrixLaneColumn(int which)
{
    if (which == 0)
        return "sgmColumn";

    return Strings::format("sgmColumn + {}u", which);
}
} // namespace eacp::GPU::ShaderEmission
