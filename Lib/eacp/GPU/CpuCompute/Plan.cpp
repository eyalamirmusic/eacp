#include "PlanBuilder.h"

#include <eacp/Core/Utils/Logging.h>

#include <atomic>

namespace eacp::GPU::CpuCompute
{
PlanBuilder::PlanBuilder(Plan& planToFill,
                         const ShaderGraph& graphToRead,
                         Plan::Options optionsToUse,
                         int mostGroupsPerBatch)
    : plan(planToFill)
    , graph(graphToRead)
    , options(optionsToUse)
    , groupLimit(mostGroupsPerBatch)
{
}

void PlanBuilder::build()
{
    if (!checkGraph() || !resolveOverrideWords())
        return;

    plan.nodes.resize(graph.nodeCount());
    marks.resize(graph.nodeCount(), 0);
    arrayUsed.resize(graph.arrays().size(), 0);
    blockReached.resize(graph.blockCount(), 0);

    buildBlock(ShaderGraph::rootBlock, 0, 0);

    if (failed())
        return;

    markReachable();

    for (auto id = 0; id < graph.nodeCount() && !failed(); ++id)
    {
        if (!plan.nodes[id].used)
            continue;

        decodeNode(id);
        markHalfRounding(id);
    }

    if (failed())
        return;

    chooseBatchWidth();
    markRamps();
    buildSchedules();
    layOut();
}

void PlanBuilder::failWith(std::string reason)
{
    if (!failed())
        plan.failure = std::move(reason);
}

namespace
{
std::uint64_t nextPlanSerial()
{
    static auto counter = std::atomic<std::uint64_t> {0};
    return counter.fetch_add(1, std::memory_order_relaxed) + 1;
}
} // namespace

Plan::Plan(const ShaderGraph& graph, Options options)
{
    if (graph.functionCount() > 0)
        build(graph.withFunctionsInlined(), options);
    else
        build(graph, options);
}

void Plan::build(const ShaderGraph& graph, const Options& options)
{
    auto groupLimit = std::numeric_limits<int>::max();

    while (true)
    {
        *this = Plan {};
        PlanBuilder {*this, graph, options, groupLimit}.build();

        if (groupsInBatch == 1 || fitsBatchBudget(options))
            break;

        groupLimit = groupsInBatch / 2;
    }

    planSerial = nextPlanSerial();

    if (!isValid())
        LOG("eacp: the CPU executor cannot run this kernel: ", failure);
}

BufferAccess Plan::access(int slot) const
{
    return slot >= 0 && slot < slotCount ? slotAccess[static_cast<std::size_t>(slot)]
                                         : BufferAccess::Read;
}

ValueType Plan::element(int slot) const
{
    return slot >= 0 && slot < slotCount
               ? slotElement[static_cast<std::size_t>(slot)]
               : ValueType::Float;
}

bool Plan::referencesSlot(int slot) const
{
    return slot >= 0 && slot < slotCount
           && slotReferenced[static_cast<std::size_t>(slot)];
}

int Plan::structStride(int slot) const
{
    return slot >= 0 && slot < slotCount
               ? slotStructStride[static_cast<std::size_t>(slot)]
               : 0;
}

std::span<const StructComponent> Plan::structComponents(int slot) const
{
    if (structStride(slot) == 0)
        return {};

    const auto& range = slotStructComponents[static_cast<std::size_t>(slot)];
    return {structComponentList.data() + range.begin,
            static_cast<std::size_t>(range.end - range.begin)};
}

ValueType Plan::uniformType(int slot) const
{
    return slot >= 0 && slot < uniformTypes.size() ? uniformTypes[slot]
                                                   : ValueType::Float;
}

int Plan::uniformCount() const
{
    return uniformTypes.size();
}

int Plan::uniformOffset(int slot) const
{
    return uniformOffsets[slot];
}

int Plan::stepCount() const
{
    return steps.size();
}

int Plan::arrayElement(const ArrayLayout& array, int which) const
{
    return arrayElements[array.elementBegin + which];
}

std::size_t Plan::footprintBytes() const
{
    return wordCount * sizeof(Word) + 64;
}

bool Plan::fitsBatchBudget(const Options& options) const
{
    return wordCount <= PlanBuilder::maxWords
           && footprintBytes() <= options.maxBatchBytes;
}
} // namespace eacp::GPU::CpuCompute
