#pragma once

#include "ShaderDialect.h"

#include <string_view>

namespace eacp::GPU::ShaderEmission
{
int getThreadsPerGroup(const ShaderGraph& graph);
bool doesMetalCombinePartials(const ShaderGraph& graph);
constexpr std::string_view getGroupScratchName(ValueType elementType)
{
    return elementType == ValueType::UInt ? "groupScratchU" : "groupScratch";
}

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

constexpr std::string_view getGroupLaneName(Backend backend)
{
    return backend == Backend::Vulkan ? "gl_LocalInvocationIndex" : "groupLane";
}

std::string getSimdMatrixName(int slot);

std::string formatSimdMatrixScratchDeclaration(const ShaderGraph& graph,
                                               std::string_view qualifier);
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
