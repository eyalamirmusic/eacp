#include "Executor.h"

#include "CpuUniformVisitor.h"
#include "Interpreter.h"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <limits>

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

void executorSplatUniforms(const Plan& plan,
                           const Word* uniformBlock,
                           Workspace& workspace)
{
    auto stride = plan.laneStride();

    for (const auto& uniform: plan.uniformNodes())
    {
        const auto& node = plan.node(uniform.node);
        const auto* words = uniformBlock + plan.uniformOffset(uniform.index);

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

// A run of consecutive x groups in one (y, z) row: the first group's id and
// thread origin, and how many of the plan's groups per batch are real.
struct Batch
{
    Extents group {};
    Extents origin {};
    Word groups = 1;
};

// A lane's offset from the batch's origin along an axis.
std::uint32_t executorBatchCoordinates(const Plan& plan, int axis)
{
    return axis == 0 ? plan.batchX() : plan.localCoordinates(axis);
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
            const auto* local = context.lanes(executorBatchCoordinates(plan, axis));
            auto* out = context.lanes(node, component);
            auto stride = context.stride;

            for (auto lane = 0; lane < stride; ++lane)
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

// The lanes of the groups past a short last batch are real lanes of no group.
void executorMaskMissingGroups(const Context& context, Word* mask, Word groups)
{
    const auto& plan = context.plan;

    if (groups >= static_cast<Word>(plan.groupsPerBatch()))
        return;

    const auto* group = context.lanes(plan.groupInBatch());

    for (auto lane = 0; lane < context.stride; ++lane)
        mask[lane] &= Lanes::maskOf(group[lane] < groups);
}

bool executorGuardBatch(const Context& context,
                        Word* mask,
                        const Batch& batch,
                        Extents extents)
{
    const auto& plan = context.plan;
    const auto* real = context.lanes(plan.realLanes());
    auto stride = context.stride;

    Lanes::copy(mask, real, stride);
    executorMaskMissingGroups(context, mask, batch.groups);

    if (plan.guardsBounds())
    {
        auto shape = plan.groupShape();
        auto sizes = std::array<std::uint64_t, 3> {
            static_cast<std::uint64_t>(shape.x) * batch.groups,
            static_cast<std::uint64_t>(shape.y),
            static_cast<std::uint64_t>(shape.z)};

        for (auto axis = 0; axis < executorAxes(plan.rank()); ++axis)
        {
            const auto* local = context.lanes(executorBatchCoordinates(plan, axis));
            auto base = batch.origin[static_cast<std::size_t>(axis)];
            auto limit = extents[static_cast<std::size_t>(axis)];
            auto groupEnd = static_cast<std::uint64_t>(base)
                            + sizes[static_cast<std::size_t>(axis)];

            if (groupEnd <= limit)
                continue;

            for (auto lane = 0; lane < stride; ++lane)
                mask[lane] &= Lanes::maskOf(base + local[lane] < limit);
        }
    }

    return Lanes::anyActive(mask, stride);
}

void executorEvaluate(const Context& context, const Plan::Range& schedule)
{
    evaluateRange(context, schedule.begin, schedule.end);
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

void executorFillGroupIdRow(const Context& context,
                            Word* out,
                            int axis,
                            Extents group)
{
    auto base = group[static_cast<std::size_t>(axis)];

    if (axis != 0 || context.plan.groupsPerBatch() == 1)
    {
        Lanes::fill(out, base, context.stride);
        return;
    }

    const auto* groupInBatch = context.lanes(context.plan.groupInBatch());

    for (auto lane = 0; lane < context.stride; ++lane)
        out[lane] = base + groupInBatch[lane];
}

void executorFillGroupIds(const Context& context, Extents group)
{
    const auto& plan = context.plan;

    for (const auto& groupId: plan.groupIdNodes())
    {
        const auto& node = plan.node(groupId.node);

        for (auto component = 0; component < node.components; ++component)
        {
            auto axis = groupId.index == allComponents ? component : groupId.index;
            executorFillGroupIdRow(
                context, context.lanes(node, component), axis, group);
        }
    }
}

void executorClearGroupMemory(const Context& context)
{
    const auto& plan = context.plan;

    if (plan.sharedWordCount() > 0)
        Lanes::fill(context.lanes(plan.sharedWords()), 0u, plan.sharedWordCount());

    if (plan.fragmentWordCount() > 0)
        Lanes::fill(
            context.lanes(plan.fragmentWords()), 0u, plan.fragmentWordCount());
}

void executorRunBatch(const Context& context, const Batch& batch, Extents extents)
{
    executorFillThreadIds(context, batch.origin);

    auto root = MaskFrame {context.lanes(context.plan.maskFrame(0)), nullptr};

    if (!executorGuardBatch(context, root.mask, batch, extents))
        return;

    executorFillGroupIds(context, batch.group);
    executorEvaluate(context, context.plan.groupSchedule());
    executorClearGroupMemory(context);
    executorEvaluateArrays(context);
    runBlock(context, context.plan.rootBlock(), root, 0, nullptr);
}

// Groups [first, end) in order, x fastest, a batch of consecutive x groups at a
// time; a batch never crosses into the next (y, z) row, and one cut short by
// the row or the range masks off the groups it does not have.
void executorRunGroups(const Context& context,
                       Extents groups,
                       Extents extents,
                       std::uint64_t first,
                       std::uint64_t end)
{
    auto shape = context.plan.groupShape();
    auto sizes = Extents {static_cast<Word>(shape.x),
                          static_cast<Word>(shape.y),
                          static_cast<Word>(shape.z)};
    auto perBatch = static_cast<std::uint64_t>(context.plan.groupsPerBatch());
    auto rowLength = static_cast<std::uint64_t>(groups[0]);

    while (first < end)
    {
        auto row = first / rowLength;
        auto x = first % rowLength;
        auto count = std::min({perBatch, rowLength - x, end - first});
        auto y = static_cast<Word>(row % groups[1]);
        auto z = static_cast<Word>(row / groups[1]);

        auto batch = Batch {};
        batch.group = {static_cast<Word>(x), y, z};
        batch.origin = {static_cast<Word>(x) * sizes[0], y * sizes[1], z * sizes[2]};
        batch.groups = static_cast<Word>(count);
        executorRunBatch(context, batch, extents);
        first += count;
    }
}

Word executorGroupsFor(int extent, int size)
{
    return static_cast<Word>((static_cast<std::int64_t>(extent) + size - 1) / size);
}

Extents executorWords(int x, int y, int z)
{
    return {static_cast<Word>(x), static_cast<Word>(y), static_cast<Word>(z)};
}

std::int64_t executorTotalGroups(Extents groups)
{
    constexpr auto most = std::numeric_limits<std::int64_t>::max();
    auto total = std::uint64_t {1};

    for (auto count: groups)
    {
        if (count != 0 && total > static_cast<std::uint64_t>(most) / count)
            return most;

        total *= count;
    }

    return static_cast<std::int64_t>(total);
}
} // namespace

Executor::Executor(ComputeKernel& kernelToRun, Plan::Options options)
    : executionPlan(kernelToRun.graph(), options)
    , scratch(executionPlan)
    , kernel(&kernelToRun)
{
    uniformBlock.resize(executionPlan.uniformBlockWords(), 0u);
}

Executor::Executor(const ShaderGraph& graph, Plan::Options options)
    : executionPlan(graph, options)
    , scratch(executionPlan)
{
    uniformBlock.resize(executionPlan.uniformBlockWords(), 0u);
}

bool Executor::setUniform(int slot, const void* data, int bytes)
{
    if (!isValid() || data == nullptr || slot < 0
        || slot >= executionPlan.uniformCount()
        || bytes != byteSize(executionPlan.uniformType(slot)))
        return false;

    std::memcpy(uniformBlock.data() + executionPlan.uniformOffset(slot),
                data,
                static_cast<std::size_t>(bytes));
    return true;
}

bool Executor::dispatch(const Bindings& bindings, int count)
{
    return runWhole(prepareDispatch(bindings, count));
}

bool Executor::dispatch(const Bindings& bindings, int width, int height)
{
    return runWhole(prepareDispatch(bindings, width, height));
}

bool Executor::dispatch(const Bindings& bindings, int width, int height, int depth)
{
    return runWhole(prepareDispatch(bindings, width, height, depth));
}

bool Executor::dispatchIndirect(const Bindings& bindings,
                                std::span<const std::uint32_t> arguments,
                                int guardCount,
                                int offsetInElements)
{
    return runWhole(
        prepareDispatchIndirect(bindings, arguments, guardCount, offsetInElements));
}

PreparedDispatch Executor::prepareDispatch(const Bindings& bindings, int count)
{
    return prepare(bindings, DispatchRank::OneD, {count, 1, 1});
}

PreparedDispatch
    Executor::prepareDispatch(const Bindings& bindings, int width, int height)
{
    return prepare(bindings, DispatchRank::TwoD, {width, height, 1});
}

PreparedDispatch Executor::prepareDispatch(const Bindings& bindings,
                                           int width,
                                           int height,
                                           int depth)
{
    return prepare(bindings, DispatchRank::ThreeD, {width, height, depth});
}

PreparedDispatch
    Executor::prepare(const Bindings& bindings, DispatchRank rank, GridSize extents)
{
    auto prepared = PreparedDispatch {};

    if (!isValid() || rank != executionPlan.rank()
        || !resolveSlots(bindings, prepared))
        return prepared;

    prepared.valid = true;
    prepared.planSerial = executionPlan.serial();

    for (auto extent: extents)
        if (extent <= 0)
            return prepared;

    auto shape = executionPlan.groupShape();
    auto axes = executorAxes(rank);

    prepared.extents = executorWords(extents[0], extents[1], extents[2]);
    prepared.groupsPerAxis = {
        executorGroupsFor(extents[0], shape.x),
        axes >= 2 ? executorGroupsFor(extents[1], shape.y) : 1u,
        axes >= 3 ? executorGroupsFor(extents[2], shape.z) : 1u};
    prepared.totalGroups = executorTotalGroups(prepared.groupsPerAxis);
    readUniforms(prepared);
    return prepared;
}

PreparedDispatch
    Executor::prepareDispatchIndirect(const Bindings& bindings,
                                      std::span<const std::uint32_t> arguments,
                                      int guardCount,
                                      int offsetInElements)
{
    auto prepared = PreparedDispatch {};

    if (!isValid() || executionPlan.rank() != DispatchRank::OneD
        || !resolveSlots(bindings, prepared))
        return prepared;

    prepared.valid = true;
    prepared.planSerial = executionPlan.serial();

    constexpr auto argumentWords = sizeof(DispatchArguments) / sizeof(Word);
    auto offset = static_cast<std::size_t>(offsetInElements);

    if (offsetInElements < 0 || arguments.size() < argumentWords
        || offset > arguments.size() - argumentWords)
        return prepared;

    auto groups =
        Extents {arguments[offset], arguments[offset + 1], arguments[offset + 2]};

    for (auto count: groups)
        if (count == 0)
            return prepared;

    prepared.extents = executorWords(guardCount > 0 ? guardCount : 0, 1, 1);
    prepared.groupsPerAxis = groups;
    prepared.totalGroups = executorTotalGroups(groups);
    readUniforms(prepared);
    return prepared;
}

bool Executor::dispatchGroups(const PreparedDispatch& prepared,
                              std::int64_t first,
                              std::int64_t count,
                              Workspace& workspace) const
{
    auto serial = executionPlan.serial();

    if (!prepared.valid || prepared.planSerial != serial
        || workspace.planSerial() != serial)
        return false;

    auto total = prepared.totalGroups;

    if (count <= 0 || first >= total)
        return true;

    auto begin = std::max<std::int64_t>(first, 0);
    auto end = first >= total - count ? total : first + count;

    if (begin >= end)
        return true;

    auto context =
        Context {executionPlan, workspace.words(), executionPlan.laneStride()};

    for (auto index = 0; index < executionPlan.storageSlotCount(); ++index)
    {
        const auto& slot = prepared.slots[static_cast<std::size_t>(index)];
        context.slots[static_cast<std::size_t>(index)] = {slot.data, slot.count};
    }

    executorSplatUniforms(executionPlan, prepared.uniforms.data(), workspace);
    executorSplatExtents(executionPlan, workspace, prepared.extents);
    executorEvaluate(context, executionPlan.dispatchSchedule());
    executorRunGroups(context,
                      prepared.groupsPerAxis,
                      prepared.extents,
                      static_cast<std::uint64_t>(begin),
                      static_cast<std::uint64_t>(end));
    return true;
}

bool Executor::runWhole(const PreparedDispatch& prepared)
{
    return dispatchGroups(prepared, 0, prepared.groupCount(), scratch);
}

bool Executor::resolveSlots(const Bindings& bindings,
                            PreparedDispatch& prepared) const
{
    for (auto index = 0; index < executionPlan.storageSlotCount(); ++index)
    {
        if (!executionPlan.referencesSlot(index))
            continue;

        const auto& slot = bindings.slot(index);

        if (!executorSlotMatches(executionPlan, slot, index))
            return false;

        auto& bound = prepared.slots[static_cast<std::size_t>(index)];
        bound.data = static_cast<std::byte*>(const_cast<void*>(slot.data));
        bound.count = slot.data != nullptr ? slot.count : 0u;
    }

    return true;
}

void Executor::readUniforms(PreparedDispatch& prepared)
{
    if (kernel != nullptr)
    {
        auto visitor =
            CpuUniformVisitor {kernel->graph(), executionPlan, uniformBlock.data()};
        kernel->visitMembers(visitor);
    }

    std::copy(uniformBlock.begin(), uniformBlock.end(), prepared.uniforms.begin());
}
} // namespace eacp::GPU::CpuCompute
