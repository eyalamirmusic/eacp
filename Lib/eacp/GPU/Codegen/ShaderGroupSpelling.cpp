#include "ShaderGroupSpelling.h"

namespace eacp::GPU::ShaderEmission
{

// How many threads one group holds, whatever its rank - what a group reduction
// folds over and what its scratch is sized for.
int threadsPerGroup(const ShaderGraph& graph)
{
    return graph.threadGroupShape().threadCount();
}

// The first halving step of a tree over that many threads: the largest power of
// two below the count, so a count that is not one still folds its tail in first.
int reductionStride(int threads)
{
    auto stride = 1;

    while (stride * 2 < threads)
        stride *= 2;

    return stride;
}

// How many threads one fold spans: the whole group, or one SIMD group of it -
// and a group smaller than a SIMD group is its own SIMD group, so the narrow
// scope never reaches past what was dispatched.
int reductionWidth(const ShaderGraph& graph, ReductionScope scope)
{
    auto threads = threadsPerGroup(graph);

    if (scope == ReductionScope::Group)
        return threads;

    return threads < simdGroupWidth ? threads : simdGroupWidth;
}

// Whether a fold spans the whole group either because that is its scope or
// because the group is one SIMD group and the two are the same set of threads.
bool spansWholeGroup(const ShaderGraph& graph, ReductionScope scope)
{
    return reductionWidth(graph, scope) == threadsPerGroup(graph);
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
bool metalFoldsInOneInstruction(ReductionScope scope)
{
    return scope == ReductionScope::Simd;
}

// So a kernel whose folds are all SIMD-scoped on Metal declares neither the
// scratch nor the three SIMD-group builtins the combine walks.
bool metalCombinesPartials(const ShaderGraph& graph)
{
    return !graph.wholeGroupReductionTypes().empty();
}

// The scratch a reduction stages its partials in, one array per element type
// folded. Named rather than slotted because it is the emitter's own and not
// something the kernel declared.
const char* groupScratchName(ValueType elementType)
{
    return elementType == ValueType::UInt ? "groupScratchU" : "groupScratch";
}

// The MSL SIMD-group reduction each fold starts from.
const char* metalSimdReduction(GroupReduction operation)
{
    switch (operation)
    {
        case GroupReduction::Sum:
            return "simd_sum";
        case GroupReduction::Max:
            return "simd_max";
        case GroupReduction::Min:
            return "simd_min";
    }

    return "simd_sum";
}

// Two partials combined, spelled identically in all three dialects.
std::string foldedPair(GroupReduction operation,
                       const std::string& left,
                       const std::string& right)
{
    switch (operation)
    {
        case GroupReduction::Sum:
            return left + " + " + right;
        case GroupReduction::Max:
            return "max(" + left + ", " + right + ")";
        case GroupReduction::Min:
            return "min(" + left + ", " + right + ")";
    }

    return left + " + " + right;
}

// The barrier itself, shared by the barrier statement and by the reductions
// that bracket their scratch with one.
std::string barrierStatement(Backend backend, const std::string& indent)
{
    if (backend == Backend::Vulkan)
        return indent + "memoryBarrierShared();\n" + indent + "barrier();\n";

    return indent
           + std::string(backend == Backend::Metal
                             ? "threadgroup_barrier(mem_flags::mem_threadgroup);\n"
                             : "GroupMemoryBarrierWithGroupSync();\n");
}

// The flat position of a thread within its group, which the tree indexes its
// scratch by. Both dialects that take the tree have a builtin for it, so
// neither derives one from the three-component local id.
const char* groupLaneName(Backend backend)
{
    return backend == Backend::Vulkan ? "gl_LocalInvocationIndex" : "groupLane";
}

// A SIMD-group matrix fragment's name. Its own numbering, so it collides with
// neither the variables nor the shared arrays.
std::string simdMatrixName(int slot)
{
    return "sgm" + std::to_string(slot);
}

// Where a fragment's patch lives, spelled as the emitted source names it.
std::string simdMatrixMemoryName(const Statement& statement)
{
    return (statement.memory == SimdMatrixMemory::Shared ? "s" : "buffer")
           + std::to_string(statement.bufferSlot);
}

// The MSL type a fragment of each element is, and the element type its load
// reinterprets the buffer's pointer as. A packed fragment stays packed right
// through the product: MSL's simdgroup_multiply_accumulate takes mixed operand
// types into a float accumulator, which is the instruction the hardware has and
// the reason there is no widening step to emit between the two.
const char* metalSimdMatrixType(SimdMatrixElement element)
{
    switch (element)
    {
        case SimdMatrixElement::Half:
            return "simdgroup_half8x8";
        case SimdMatrixElement::BFloat16:
            return "simdgroup_bfloat8x8";
        case SimdMatrixElement::Float:
            break;
    }

    return "simdgroup_float8x8";
}

const char* metalPackedElementType(SimdMatrixElement element)
{
    return element == SimdMatrixElement::Half ? "half" : "bfloat";
}

// What the fallback widens one packed element with - the same helper a scalar
// InputBuffer::readHalf or readBFloat16 goes through, so the arithmetic that
// produces a lane's pair here is the arithmetic every other packed read in the
// shader uses.
const char* packedSimdMatrixHelper(SimdMatrixElement element)
{
    return element == SimdMatrixElement::Half ? "eacpReadHalf" : "eacpReadBFloat16";
}

// How the two fallback dialects read a buffer's float element as the word its
// bits are. The buffer is declared float whatever it holds, which is what the
// packed scalar reads already assume. Metal never asks: a packed patch is a
// fragment of its own type there, loaded through a reinterpreted pointer.
const char* bitsOfFloat(Backend backend)
{
    return backend == Backend::Vulkan ? "floatBitsToUint" : "asuint";
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
const char* simdMatrixLaneType(Backend backend)
{
    return typeName(backend, ValueType::Float2);
}

// The scratch a product stages its two operands in, whole, so each lane can
// read the row and the columns other lanes hold: two fragments per SIMD group,
// one slice per SIMD group of the threadgroup.
constexpr int simdMatrixScratchPerGroup = 2 * simdMatrixSize * simdMatrixSize;

int simdMatrixScratchElements(const ShaderGraph& graph)
{
    return graph.threadGroupShape().threadCount() / simdGroupWidth
           * simdMatrixScratchPerGroup;
}

// The scratch declared, under the storage qualifier the dialect gives
// threadgroup memory, in a kernel that holds any fragment at all.
std::string simdMatrixScratchDeclaration(const ShaderGraph& graph,
                                         const std::string& qualifier)
{
    if (graph.simdMatrixCount() == 0)
        return {};

    return qualifier + " float " + simdMatrixScratchName + "["
           + std::to_string(simdMatrixScratchElements(graph)) + "];\n";
}

// Whether a kernel declares any threadgroup memory of its own or the
// emitter's - what the blank line after those declarations is for.
bool declaresGroupMemory(const ShaderGraph& graph)
{
    return graph.sharedArrays().size() > 0 || graph.usesGroupReduction()
           || graph.simdMatrixCount() > 0;
}

// What the fallback addresses a fragment by, declared once at the top of any
// kernel that holds one: the lane within its notional SIMD group, the row and
// the first column of the pair that lane holds, and where that group's slice
// of the scratch starts.
std::string simdMatrixPreamble(Backend backend)
{
    auto lane = std::string(groupLaneName(backend));
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
