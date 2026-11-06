#pragma once

#include "ShaderDialect.h"
#include "ShaderText.h"

#include <string_view>

// The spellings of the group-wide kernel operations: reductions over a
// threadgroup or a SIMD group, and the SIMD-group matrix fragments with the
// lane-spread fallback the backends without them take.

namespace eacp::GPU::ShaderEmission
{
// The scratch a SIMD-group matrix product stages its operands in. Named rather
// than slotted, like the reduction's, because it is the emitter's own.
inline constexpr auto simdMatrixScratchName = "sgmScratch";

int getThreadsPerGroup(const ShaderGraph& graph);
int getReductionStride(int threads);
int getReductionWidth(const ShaderGraph& graph, ReductionScope scope);
bool doesFoldSpanWholeGroup(const ShaderGraph& graph, ReductionScope scope);
bool doesMetalFoldInOneInstruction(ReductionScope scope);
bool doesMetalCombinePartials(const ShaderGraph& graph);
// The scratch a reduction stages its partials in, one array per element type
// folded. Named rather than slotted because it is the emitter's own and not
// something the kernel declared.
constexpr std::string_view getGroupScratchName(ValueType elementType)
{
    return elementType == ValueType::UInt ? "groupScratchU" : "groupScratch";
}

// The MSL SIMD-group reduction each fold starts from.
constexpr std::string_view getMetalSimdReduction(GroupReduction operation)
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
constexpr std::string formatFoldedPair(GroupReduction operation,
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
constexpr std::string formatBarrierStatement(Backend backend,
                                             const std::string& indent)
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
constexpr std::string_view getGroupLaneName(Backend backend)
{
    return backend == Backend::Vulkan ? "gl_LocalInvocationIndex" : "groupLane";
}

// A SIMD-group matrix fragment's name. Its own numbering, so it collides with
// neither the variables nor the shared arrays.
constexpr std::string getSimdMatrixName(int slot)
{
    return "sgm" + formatDecimal(slot);
}

std::string getSimdMatrixMemoryName(const Statement& statement);
// The MSL type a fragment of each element is, and the element type its load
// reinterprets the buffer's pointer as. A packed fragment stays packed right
// through the product: MSL's simdgroup_multiply_accumulate takes mixed operand
// types into a float accumulator, which is the instruction the hardware has and
// the reason there is no widening step to emit between the two.
constexpr std::string_view getMetalSimdMatrixType(SimdMatrixElement element)
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

constexpr std::string_view getMetalPackedElementType(SimdMatrixElement element)
{
    return element == SimdMatrixElement::Half ? "half" : "bfloat";
}

// What the fallback widens one packed element with - the same helper a scalar
// InputBuffer::readHalf or readBFloat16 goes through, so the arithmetic that
// produces a lane's pair here is the arithmetic every other packed read in the
// shader uses.
constexpr std::string_view getPackedSimdMatrixHelper(SimdMatrixElement element)
{
    return element == SimdMatrixElement::Half ? "eacpReadHalf" : "eacpReadBFloat16";
}

// How the two fallback dialects read a buffer's float element as the word its
// bits are. The buffer is declared float whatever it holds, which is what the
// packed scalar reads already assume. Metal never asks: a packed patch is a
// fragment of its own type there, loaded through a reinterpreted pointer.
constexpr std::string_view getBitsOfFloatFunction(Backend backend)
{
    return backend == Backend::Vulkan ? "floatBitsToUint" : "asuint";
}

std::string_view getSimdMatrixLaneType(Backend backend);
std::string formatSimdMatrixScratchDeclaration(const ShaderGraph& graph,
                                               const std::string& qualifier);
bool doesDeclareGroupMemory(const ShaderGraph& graph);
std::string formatSimdMatrixPreamble(Backend backend);

enum class StagedOperand
{
    Left,
    Right
};

// An element of an operand a product staged in the scratch: the left fragment
// at the start of this SIMD group's slice, the right one a fragment further on.
constexpr std::string printStagedSimdMatrixElement(StagedOperand operand,
                                                   std::string_view element)
{
    auto text = std::string(simdMatrixScratchName) + "[sgmBase + ";

    if (operand == StagedOperand::Right)
        text += formatDecimal(simdMatrixSize * simdMatrixSize) + "u + ";

    text += element;
    text += "]";
    return text;
}

// The first or second of the pair of columns a lane holds.
constexpr std::string printSimdMatrixLaneColumn(int which)
{
    if (which == 0)
        return "sgmColumn";

    return "sgmColumn + " + formatDecimal(which) + "u";
}
} // namespace eacp::GPU::ShaderEmission
