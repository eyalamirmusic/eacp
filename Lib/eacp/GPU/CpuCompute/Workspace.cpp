#include "Workspace.h"

#include <cstdint>
#include <utility>

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
    auto paddingWords = (aligned - address) / sizeof(Word);
    return start + paddingWords;
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
        auto isReal = lane < plan.batchLanes();
        auto local = lane % plan.lanes();
        x[lane] = isReal ? static_cast<Word>(local % shape.x) : 0u;
        y[lane] = isReal ? static_cast<Word>((local / shape.x) % shape.y) : 0u;
        z[lane] = isReal ? static_cast<Word>(local / (shape.x * shape.y)) : 0u;
        real[lane] = Lanes::maskOf(isReal);
    }
}

void fillWorkspaceBatchCoordinates(const Plan& plan, Workspace& workspace)
{
    if (plan.groupsPerBatch() == 1)
        return;

    auto width = static_cast<Word>(plan.groupShape().x);
    const auto* x = workspace.at(plan.localCoordinates(0));
    auto* group = workspace.at(plan.groupInBatch());
    auto* batchX = workspace.at(plan.batchX());

    for (auto lane = 0; lane < plan.laneStride(); ++lane)
    {
        auto isReal = lane < plan.batchLanes();
        group[lane] = isReal ? static_cast<Word>(lane / plan.lanes()) : 0u;
        batchX[lane] = group[lane] * width + x[lane];
    }
}

void fillWorkspaceSimdGroupIndices(const Plan& plan, Workspace& workspace)
{
    for (auto node: plan.simdGroupIndexNodes())
    {
        auto* out = workspace.at(plan.node(node).scratch);

        for (auto lane = 0; lane < plan.laneStride(); ++lane)
            out[lane] = static_cast<Word>(lane / simdGroupWidth);
    }
}
} // namespace

Workspace::Workspace(const Plan& plan)
    : serial(plan.serial())
{
    if (!plan.isValid())
        return;

    storage.resize(static_cast<int>(plan.totalWords() + workspaceAlignmentWords),
                   0u);
    base = alignedWords(storage.data());

    fillWorkspaceConstants(plan, *this);
    fillWorkspaceLocalCoordinates(plan, *this);
    fillWorkspaceBatchCoordinates(plan, *this);
    fillWorkspaceSimdGroupIndices(plan, *this);
}

Workspace::Workspace(Workspace&& other) noexcept
    : storage(std::move(other.storage))
    , base(std::exchange(other.base, nullptr))
    , serial(std::exchange(other.serial, 0))
{
}

Workspace& Workspace::operator=(Workspace&& other) noexcept
{
    storage = std::move(other.storage);
    base = std::exchange(other.base, nullptr);
    serial = std::exchange(other.serial, 0);
    return *this;
}
} // namespace eacp::GPU::CpuCompute
