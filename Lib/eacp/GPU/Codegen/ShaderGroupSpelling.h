#pragma once

#include "ShaderDialect.h"

// The spellings of the group-wide kernel operations: reductions over a
// threadgroup or a SIMD group, and the SIMD-group matrix fragments with the
// lane-spread fallback the backends without them take.

namespace eacp::GPU::ShaderEmission
{
// The scratch a SIMD-group matrix product stages its operands in. Named rather
// than slotted, like the reduction's, because it is the emitter's own.
inline constexpr auto simdMatrixScratchName = "sgmScratch";

int threadsPerGroup(const ShaderGraph& graph);
int reductionStride(int threads);
int reductionWidth(const ShaderGraph& graph, ReductionScope scope);
bool spansWholeGroup(const ShaderGraph& graph, ReductionScope scope);
bool metalFoldsInOneInstruction(ReductionScope scope);
bool metalCombinesPartials(const ShaderGraph& graph);
const char* groupScratchName(ValueType elementType);
const char* metalSimdReduction(GroupReduction operation);
std::string foldedPair(GroupReduction operation,
                       const std::string& left,
                       const std::string& right);
std::string barrierStatement(Backend backend, const std::string& indent);
const char* groupLaneName(Backend backend);

std::string simdMatrixName(int slot);
std::string simdMatrixMemoryName(const Statement& statement);
const char* metalSimdMatrixType(SimdMatrixElement element);
const char* metalPackedElementType(SimdMatrixElement element);
const char* packedSimdMatrixHelper(SimdMatrixElement element);
const char* bitsOfFloat(Backend backend);
const char* simdMatrixLaneType(Backend backend);
std::string simdMatrixScratchDeclaration(const ShaderGraph& graph,
                                         const std::string& qualifier);
bool declaresGroupMemory(const ShaderGraph& graph);
std::string simdMatrixPreamble(Backend backend);
} // namespace eacp::GPU::ShaderEmission
