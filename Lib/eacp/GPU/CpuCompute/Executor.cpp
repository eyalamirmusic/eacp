#include "Executor.h"

#include "CpuUniformVisitor.h"
#include "Interpreter.h"

#include <cstdint>
#include <cstring>

namespace eacp::GPU::CpuCompute
{
namespace
{
using Extents = std::array<Word, 3>;

bool executorSlotMatches(const Plan& plan, const Bindings::Slot& slot, int index)
{
    return slot.bound && slot.element == plan.element(index)
           && slot.access == plan.access(index);
}

bool executorResolveSlots(const Plan& plan,
                          const Bindings& bindings,
                          std::array<SlotView, Plan::maxSlots>& views)
{
    for (auto index = 0; index < plan.storageSlotCount(); ++index)
    {
        if (!plan.referencesSlot(index))
            continue;

        const auto& slot = bindings.slot(index);

        if (!executorSlotMatches(plan, slot, index))
            return false;

        auto& view = views[static_cast<std::size_t>(index)];
        view.data = static_cast<std::byte*>(const_cast<void*>(slot.data));
        view.count = slot.data != nullptr ? slot.count : 0u;
    }

    return true;
}

void executorSplatUniforms(const Plan& plan, Workspace& workspace)
{
    auto stride = plan.laneStride();

    for (const auto& uniform: plan.uniformNodes())
    {
        const auto& node = plan.node(uniform.node);
        const auto* words = workspace.uniformWords(plan, uniform.index);

        for (auto component = 0; component < node.components; ++component)
            Lanes::fill(
                workspace.at(node.scratch
                             + static_cast<std::uint32_t>(component * stride)),
                words[component],
                stride);
    }
}

void executorSplatExtents(const Plan& plan, Workspace& workspace, Extents extents)
{
    for (const auto& extent: plan.extentNodes())
    {
        auto axis = plan.rank() == DispatchRank::OneD ? 0 : extent.index;
        auto value =
            axis >= 0 && axis < 3 ? extents[static_cast<std::size_t>(axis)] : 0u;

        Lanes::fill(
            workspace.at(plan.node(extent.node).scratch), value, plan.laneStride());
    }
}

void executorFillThreadIds(const Context& context, Extents origin)
{
    const auto& plan = context.plan;

    for (const auto& threadId: plan.threadIdNodes())
    {
        const auto& node = plan.node(threadId.node);

        for (auto component = 0; component < node.components; ++component)
        {
            auto axis = threadId.index == allComponents ? component : threadId.index;
            auto base = origin[static_cast<std::size_t>(axis)];
            const auto* local = context.lanes(plan.localCoordinates(axis));
            auto* out = context.lanes(node, component);

            for (auto lane = 0; lane < context.stride; ++lane)
                out[lane] = base + local[lane];
        }
    }
}

int executorAxes(DispatchRank rank)
{
    switch (rank)
    {
        case DispatchRank::OneD:
            return 1;
        case DispatchRank::TwoD:
            return 2;
        case DispatchRank::ThreeD:
            return 3;
    }

    return 1;
}

bool executorGuardGroup(const Context& context,
                        Word* mask,
                        Extents origin,
                        Extents extents)
{
    const auto& plan = context.plan;
    const auto* real = context.lanes(plan.realLanes());
    auto stride = context.stride;

    Lanes::copy(mask, real, stride);

    if (plan.guardsBounds())
    {
        for (auto axis = 0; axis < executorAxes(plan.rank()); ++axis)
        {
            const auto* local = context.lanes(plan.localCoordinates(axis));
            auto base = origin[static_cast<std::size_t>(axis)];
            auto limit = extents[static_cast<std::size_t>(axis)];

            for (auto lane = 0; lane < stride; ++lane)
                mask[lane] &= Lanes::maskOf(base + local[lane] < limit);
        }
    }

    return Lanes::anyActive(mask, stride);
}

void executorEvaluateArrays(const Context& context)
{
    const auto& plan = context.plan;

    for (const auto& array: plan.arrays())
    {
        if (!array.used)
            continue;

        evaluateRange(context, array.schedule.begin, array.schedule.end);

        auto words = array.components * context.stride;

        for (auto element = 0; element < array.elementCount; ++element)
        {
            const auto& node = plan.node(plan.arrayElement(array, element));
            Lanes::copy(context.lanes(array.storage
                                      + static_cast<std::uint32_t>(element * words)),
                        context.lanes(node),
                        words);
        }
    }
}

void executorRunGroup(const Context& context, Extents origin, Extents extents)
{
    executorFillThreadIds(context, origin);

    auto root = MaskFrame {context.lanes(context.plan.maskFrame(0)), nullptr};

    if (!executorGuardGroup(context, root.mask, origin, extents))
        return;

    executorEvaluateArrays(context);
    runBlock(context, context.plan.rootBlock(), root, 0, nullptr);
}

Word executorGroupsFor(int extent, int size)
{
    return static_cast<Word>((static_cast<std::int64_t>(extent) + size - 1) / size);
}

Extents executorWords(int x, int y, int z)
{
    return {static_cast<Word>(x), static_cast<Word>(y), static_cast<Word>(z)};
}
} // namespace

Executor::Executor(ComputeKernel& kernelToRun)
    : executionPlan(kernelToRun.graph())
    , scratch(executionPlan)
    , kernel(&kernelToRun)
{
}

Executor::Executor(const ShaderGraph& graph)
    : executionPlan(graph)
    , scratch(executionPlan)
{
}

bool Executor::setUniform(int slot, const void* data, int bytes)
{
    if (!isValid() || data == nullptr || slot < 0
        || slot >= executionPlan.uniformCount()
        || bytes != byteSize(executionPlan.uniformType(slot)))
        return false;

    std::memcpy(scratch.uniformWords(executionPlan, slot),
                data,
                static_cast<std::size_t>(bytes));
    return true;
}

bool Executor::dispatch(const Bindings& bindings, int count)
{
    return run(bindings, DispatchRank::OneD, {count, 1, 1});
}

bool Executor::dispatch(const Bindings& bindings, int width, int height)
{
    return run(bindings, DispatchRank::TwoD, {width, height, 1});
}

bool Executor::dispatch(const Bindings& bindings, int width, int height, int depth)
{
    return run(bindings, DispatchRank::ThreeD, {width, height, depth});
}

bool Executor::run(const Bindings& bindings, DispatchRank rank, GridSize extents)
{
    if (!isValid() || rank != executionPlan.rank())
        return false;

    auto context =
        Context {executionPlan, scratch.words(), executionPlan.laneStride()};

    if (!executorResolveSlots(executionPlan, bindings, context.slots))
        return false;

    for (auto extent: extents)
        if (extent <= 0)
            return true;

    if (kernel != nullptr)
    {
        auto visitor = CpuUniformVisitor {kernel->graph(),
                                          scratch.uniformWords(executionPlan, 0)};
        kernel->visitMembers(visitor);
    }

    auto words = executorWords(extents[0], extents[1], extents[2]);

    executorSplatUniforms(executionPlan, scratch);
    executorSplatExtents(executionPlan, scratch, words);

    auto shape = executionPlan.groupShape();
    auto sizes = executorWords(shape.x, shape.y, shape.z);
    auto axes = executorAxes(rank);

    auto groupsX = executorGroupsFor(extents[0], shape.x);
    auto groupsY = axes >= 2 ? executorGroupsFor(extents[1], shape.y) : 1u;
    auto groupsZ = axes >= 3 ? executorGroupsFor(extents[2], shape.z) : 1u;

    for (auto z = 0u; z < groupsZ; ++z)
        for (auto y = 0u; y < groupsY; ++y)
            for (auto x = 0u; x < groupsX; ++x)
                executorRunGroup(
                    context, {x * sizes[0], y * sizes[1], z * sizes[2]}, words);

    return true;
}
} // namespace eacp::GPU::CpuCompute
