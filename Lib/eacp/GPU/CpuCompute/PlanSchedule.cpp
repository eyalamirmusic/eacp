#include "PlanBuilder.h"

#include <algorithm>

namespace eacp::GPU::CpuCompute
{
namespace
{
PlanLevel getLeafLevel(ExprKind kind)
{
    switch (kind)
    {
        case ExprKind::Constant:
        case ExprKind::Uniform:
        case ExprKind::GridExtent:
            return PlanLevel::Dispatch;
        case ExprKind::GroupId:
            return PlanLevel::Group;
        default:
            return PlanLevel::Lane;
    }
}

PlanLevel getSiteLevel(int site)
{
    if (site == PlanBuilder::dispatchSite)
        return PlanLevel::Dispatch;

    return site == PlanBuilder::groupSite ? PlanLevel::Group : PlanLevel::Lane;
}

// A read whose value can change between lanes or statements whatever its index.
bool readsChangingMemory(const Plan& plan, const Plan::Node& node)
{
    switch (node.op)
    {
        case Op::BufferRead:
        case Op::BufferVectorRead:
            return plan.access(node.immediate) != BufferAccess::Read;
        case Op::ArrayRead:
        case Op::SharedRead:
        case Op::AtomicLoad:
            return true;
        default:
            return false;
    }
}
} // namespace

bool PlanBuilder::isScheduleLeaf(int id, int site, int pinned) const
{
    if (id == pinned || plan.nodes[id].op == Op::Leaf)
        return true;

    if (levels[id] != PlanLevel::Lane)
        return levels[id] != getSiteLevel(site);

    return hoistSites[id] != unhoisted && hoistSites[id] != site;
}

template <typename Emit>
void PlanBuilder::visit(int root, int site, int pinned, Emit emit)
{
    auto isVisited = [&](int id)
    { return marks[id] == stamp || isScheduleLeaf(id, site, pinned); };

    if (root < 0 || isVisited(root))
        return;

    marks[root] = stamp;
    walk.clear();
    walk.add({root, 0});

    while (!walk.empty())
    {
        auto& top = walk.back();
        const auto& node = plan.nodes[top.node];

        if (top.next >= node.argCount)
        {
            emit(top.node);
            walk.pop_back();
            continue;
        }

        auto child = plan.argument(node, top.next++);

        if (!isVisited(child))
        {
            marks[child] = stamp;
            walk.add({child, 0});
        }
    }
}

template <typename Emit>
void PlanBuilder::visitArrayElements(const Plan::ArrayLayout& layout, Emit emit)
{
    if (!layout.used)
        return;

    for (auto element = 0; element < layout.elementCount; ++element)
        visit(plan.arrayElement(layout, element), arraySite, -1, emit);
}

void PlanBuilder::describeArrays()
{
    for (auto slot = 0; slot < graph.arrays().size(); ++slot)
    {
        const auto& array = graph.arrays()[slot];
        auto layout = Plan::ArrayLayout {};
        layout.components = componentCount(array.elementType);
        layout.elementBegin = plan.arrayElements.size();
        layout.elementCount = array.elements.size();
        layout.used = arrayUsed[slot] != 0;

        for (auto element: array.elements)
            plan.arrayElements.add(element);

        plan.arrayLayouts.add(layout);
    }
}

// Returns the last step of the block's subtree, or -1 when it has none. Step
// ids run in program order, a compound step's before its bodies'.
int PlanBuilder::mapBlock(int block, int depth)
{
    blockDepths[block] = depth;
    auto last = -1;
    const auto& range = plan.blocks[block];

    for (auto position = range.begin; position < range.end; ++position)
    {
        auto stepId = plan.blockSteps[position];
        const auto& step = plan.steps[stepId];
        auto subtreeLast = stepId;
        stepBlocks[stepId] = block;

        for (auto body: {step.body, step.elseBody})
        {
            if (body < 0)
                continue;

            blockOwners[body] = stepId;
            subtreeLast = std::max(subtreeLast, mapBlock(body, depth + 1));
        }

        subtreeLasts[stepId] = subtreeLast;
        last = std::max(last, subtreeLast);
    }

    return last;
}

void PlanBuilder::mapBlocks()
{
    stepBlocks.resize(plan.steps.size(), plan.rootBlock());
    subtreeLasts.resize(plan.steps.size(), 0);
    blockOwners.resize(plan.blocks.size(), -1);
    blockDepths.resize(plan.blocks.size(), 0);
    mapBlock(plan.rootBlock(), 0);
}

int PlanBuilder::findCommonBlock(int a, int b) const
{
    while (a != b)
    {
        if (blockDepths[a] >= blockDepths[b])
            a = stepBlocks[blockOwners[a]];
        else
            b = stepBlocks[blockOwners[b]];
    }

    return a;
}

PlanLevel PlanBuilder::calculateNodeLevel(int id) const
{
    const auto& node = plan.nodes[id];

    if (!node.used || !graph.isPure(id))
        return PlanLevel::Lane;

    if (node.op == Op::Leaf)
        return getLeafLevel(graph.expr(id).kind);

    if (readsChangingMemory(plan, node))
        return PlanLevel::Lane;

    return joinArgumentLevels(id, node);
}

// The widest level among the arguments, or Lane when one is not yet levelled.
PlanLevel PlanBuilder::joinArgumentLevels(int id, const Plan::Node& node) const
{
    auto level = PlanLevel::Dispatch;

    for (auto which = 0; which < node.argCount; ++which)
    {
        auto argument = plan.argument(node, which);

        if (argument >= id)
            return PlanLevel::Lane;

        level = std::max(level, levels[argument]);
    }

    return level;
}

void PlanBuilder::computeLevels()
{
    levels.resize(graph.nodeCount(), PlanLevel::Lane);
    hoistSites.resize(graph.nodeCount(), unhoisted);

    for (auto id = 0; id < graph.nodeCount(); ++id)
        levels[id] = calculateNodeLevel(id);
}

void PlanBuilder::countUse(int id, int site)
{
    if (levels[id] != PlanLevel::Lane || !graph.isPure(id))
        return;

    auto block = site >= 0 ? stepBlocks[site] : plan.rootBlock();

    if (useCounts[id]++ == 0)
    {
        firstUses[id] = site;
        useBlocks[id] = block;
        return;
    }

    useBlocks[id] = findCommonBlock(useBlocks[id], block);
}

// The sites each pure node would be evaluated at were every site to evaluate
// its own tree; sites are counted in program order, arrays first.
void PlanBuilder::collectUses()
{
    useCounts.resize(graph.nodeCount(), 0);
    firstUses.resize(graph.nodeCount(), unhoisted);
    useBlocks.resize(graph.nodeCount(), plan.rootBlock());

    auto countAt = [this](int site)
    { return [this, site](int id) { countUse(id, site); }; };

    ++stamp;

    for (const auto& layout: plan.arrayLayouts)
        visitArrayElements(layout, countAt(arraySite));

    for (auto stepId = 0; stepId < plan.steps.size(); ++stepId)
    {
        ++stamp;

        for (auto root: pending[stepId].roots)
            visit(root, stepId, pending[stepId].pinned, countAt(stepId));
    }
}

// The step of the block that holds the site, or the site itself.
int PlanBuilder::findSiteWithin(int site, int block) const
{
    if (site < 0)
        return site;

    while (stepBlocks[site] != block)
        site = blockOwners[stepBlocks[site]];

    return site;
}

// A pure node two sites evaluate is evaluated once, at the first step of the
// innermost block holding every use that holds one: that step runs before
// every use, and a body that may run zero times holds none of them unless it
// holds all of them.
void PlanBuilder::chooseHoistSites()
{
    hoistedAt.resize(plan.steps.size());

    for (auto id = 0; id < graph.nodeCount(); ++id)
    {
        if (useCounts[id] < 2)
            continue;

        auto site = findSiteWithin(firstUses[id], useBlocks[id]);
        hoistSites[id] = site;

        if (site >= 0)
            hoistedAt[site].add(id);
    }
}

void PlanBuilder::scheduleLevel(PlanLevel level, int site, Plan::Range& range)
{
    ++stamp;
    range.begin = plan.scheduleList.size();

    for (auto id = 0; id < graph.nodeCount(); ++id)
        if (plan.nodes[id].used && levels[id] == level)
            visit(id, site, -1, [this](int node) { appendToSchedule(node); });

    range.end = plan.scheduleList.size();
}

void PlanBuilder::scheduleArrays()
{
    ++stamp;

    for (auto& layout: plan.arrayLayouts)
    {
        layout.schedule.begin = plan.scheduleList.size();
        visitArrayElements(layout, [this](int node) { appendToSchedule(node); });
        layout.schedule.end = plan.scheduleList.size();
    }
}

void PlanBuilder::scheduleSteps()
{
    auto append = [this](int node) { appendToSchedule(node); };

    for (auto stepId = 0; stepId < plan.steps.size(); ++stepId)
    {
        const auto& roots = pending[stepId];
        ++stamp;

        auto& step = plan.steps[stepId];
        step.scheduleBegin = plan.scheduleList.size();

        for (auto root: roots.roots)
            visit(root, stepId, roots.pinned, append);

        for (auto hoisted: hoistedAt[stepId])
            visit(hoisted, stepId, roots.pinned, append);

        step.scheduleEnd = plan.scheduleList.size();
    }
}

void PlanBuilder::buildSchedules()
{
    describeArrays();
    mapBlocks();
    computeLevels();
    collectUses();
    chooseHoistSites();

    scheduleLevel(PlanLevel::Dispatch, dispatchSite, plan.dispatchRange);
    scheduleLevel(PlanLevel::Group, groupSite, plan.groupRange);
    scheduleArrays();
    scheduleSteps();
}
} // namespace eacp::GPU::CpuCompute
