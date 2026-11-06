#pragma once

#include "ShaderDialect.h"

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

std::string formatFoldedPair(GroupReduction operation,
                             const std::string& left,
                             const std::string& right);

std::string formatBarrierStatement(Backend backend, const std::string& indent);

// The flat position of a thread within its group, which the tree indexes its
// scratch by. Both dialects that take the tree have a builtin for it, so
// neither derives one from the three-component local id.
constexpr std::string_view getGroupLaneName(Backend backend)
{
    return backend == Backend::Vulkan ? "gl_LocalInvocationIndex" : "groupLane";
}

std::string getSimdMatrixName(int slot);

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
bool hasGroupMemoryDeclarations(const ShaderGraph& graph);
std::string formatSimdMatrixPreamble(Backend backend);

enum class StagedOperand
{
    Left,
    Right
};

std::string printStagedSimdMatrixElement(StagedOperand operand,
                                         std::string_view element);

std::string printSimdMatrixLaneColumn(int which);
} // namespace eacp::GPU::ShaderEmission
