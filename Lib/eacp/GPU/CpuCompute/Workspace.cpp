#include "Workspace.h"

#include <cstdint>

namespace eacp::GPU::CpuCompute
{
namespace
{
constexpr std::size_t workspaceAlignmentWords = 16;

Word* alignedWords(Word* start)
{
    auto address = reinterpret_cast<std::uintptr_t>(start);
    auto alignment = workspaceAlignmentWords * sizeof(Word);
    auto aligned = (address + alignment - 1) / alignment * alignment;
    return start + (aligned - address) / sizeof(Word);
}

void fillWorkspaceConstants(const Plan& plan, Workspace& workspace)
{
    auto stride = plan.laneStride();

    for (const auto& constant: plan.constants())
        Lanes::fill(
            workspace.at(plan.node(constant.node).scratch), constant.word, stride);
}

void fillWorkspaceLocalCoordinates(const Plan& plan, Workspace& workspace)
{
    auto shape = plan.groupShape();
    auto stride = plan.laneStride();
    auto* x = workspace.at(plan.localCoordinates(0));
    auto* y = workspace.at(plan.localCoordinates(1));
    auto* z = workspace.at(plan.localCoordinates(2));
    auto* real = workspace.at(plan.realLanes());

    for (auto lane = 0; lane < stride; ++lane)
    {
        auto isReal = lane < plan.lanes();
        x[lane] = isReal ? static_cast<Word>(lane % shape.x) : 0u;
        y[lane] = isReal ? static_cast<Word>((lane / shape.x) % shape.y) : 0u;
        z[lane] = isReal ? static_cast<Word>(lane / (shape.x * shape.y)) : 0u;
        real[lane] = Lanes::maskOf(isReal);
    }
}
} // namespace

Workspace::Workspace(const Plan& plan)
{
    if (!plan.isValid())
        return;

    storage.resize(static_cast<int>(plan.totalWords() + workspaceAlignmentWords),
                   0u);
    base = alignedWords(storage.data());

    fillWorkspaceConstants(plan, *this);
    fillWorkspaceLocalCoordinates(plan, *this);
}
} // namespace eacp::GPU::CpuCompute
