#include "ShaderGroupSpelling.h"

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
    return (statement.memory == SimdMatrixMemory::Shared ? "s" : "buffer")
           + std::to_string(statement.bufferSlot);
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

// The two-component type a lane's share is declared as.
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

    return qualifier + " float " + simdMatrixScratchName + "["
           + std::to_string(getSimdMatrixScratchElementCount(graph)) + "];\n";
}

// Whether a kernel declares any threadgroup memory of its own or the
// emitter's - what the blank line after those declarations is for.
bool doesDeclareGroupMemory(const ShaderGraph& graph)
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
    auto lane = std::string(getGroupLaneName(backend));
    auto width = std::to_string(simdGroupWidth);
    auto perRow = std::to_string(simdMatrixLanesPerRow);

    auto source = "    uint sgmLane = " + lane + " % " + width + "u;\n";
    source += "    uint sgmRow = sgmLane / " + perRow + "u;\n";
    source += "    uint sgmColumn = (sgmLane % " + perRow + "u) * "
              + std::to_string(simdMatrixLaneElements) + "u;\n";
    source += "    uint sgmBase = (" + lane + " / " + width + "u) * "
              + std::to_string(simdMatrixScratchPerGroup) + "u;\n";
    return source;
}

} // namespace eacp::GPU::ShaderEmission
