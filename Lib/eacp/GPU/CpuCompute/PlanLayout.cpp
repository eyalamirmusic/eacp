#include "PlanBuilder.h"

#include <algorithm>

namespace eacp::GPU::CpuCompute
{
namespace
{
constexpr int laneAlignment = 16;
constexpr int mostComponents = 16;
constexpr std::uint64_t wordRange = std::uint64_t {1} << 32;

std::uint32_t roundUp(int value, int multiple)
{
    return static_cast<std::uint32_t>((value + multiple - 1) / multiple * multiple);
}

bool ownsScratch(ExprKind kind)
{
    return kind != ExprKind::VarRead && kind != ExprKind::LocalId;
}

// Times: schedule position p is 2p, and a step's commit, which reads its roots
// after its schedule and before any body, is the odd time after its last
// position.
int getCommitTime(int scheduleEnd)
{
    return 2 * scheduleEnd - 1;
}

// A scalar integer node whose value on real lane l is its value on lane 0 plus
// l * scale, mod 2^32; constant also knows lane 0's value.
struct LaneForm
{
    bool linear = false;
    bool constant = false;
    Word scale = 0;
    Word value = 0;
};

LaneForm getLaneInvariantForm()
{
    return {true, false, 0u, 0u};
}

LaneForm getConstantForm(Word value)
{
    return {true, true, 0u, value};
}

bool isXAxis(int index)
{
    return index == 0 || index == allComponents;
}

LaneForm calculateSumForm(const LaneForm& a, const LaneForm& b, Word sign)
{
    if (!a.linear || !b.linear)
        return {};

    return {true,
            a.constant && b.constant,
            a.scale + sign * b.scale,
            a.value + sign * b.value};
}

LaneForm calculateProductForm(const LaneForm& a, const LaneForm& b)
{
    if (a.constant && b.constant)
        return getConstantForm(a.value * b.value);

    if (a.linear && b.constant)
        return {true, false, a.scale * b.value, 0u};

    if (b.linear && a.constant)
        return {true, false, b.scale * a.value, 0u};

    return {};
}

LaneForm calculateShiftForm(const LaneForm& a, const LaneForm& amount)
{
    if (!a.linear || !amount.constant)
        return {};

    auto shift = amount.value & 31u;
    return {true, a.constant, a.scale << shift, a.value << shift};
}

// The lane form of every node, in id order, and the ramps and invariant
// divisors it proves.
class LaneFormAnalysis
{
public:
    LaneFormAnalysis(const Plan& planToRead, const ShaderGraph& graphToRead)
        : plan(planToRead)
        , graph(graphToRead)
    {
        forms.resize(graph.nodeCount(), LaneForm {});

        for (auto id = 0; id < graph.nodeCount(); ++id)
            forms[id] = calculateNodeForm(id);
    }

    void markRampRead(Plan::Node& node, int id) const
    {
        if (node.op != Op::BufferRead && node.op != Op::BufferVectorRead)
            return;

        auto index = getArgumentForm(id, node, 0);

        if (index.linear && calculateRampSpan(index.scale) < wordRange)
        {
            node.ramp = true;
            node.rampScale = index.scale;
        }
    }

    void markInvariantDivisor(Plan::Node& node, int id) const
    {
        if (node.op != Op::DivU && node.op != Op::RemU)
            return;

        auto divisor = getArgumentForm(id, node, 1);
        node.invariantDivisor = divisor.linear && divisor.scale == 0;
    }

    void markRampStore(Plan::Step& step) const
    {
        if (step.kind != StatementKind::Store
            && step.kind != StatementKind::VectorStore)
            return;

        const auto& index = forms[step.index];
        auto width = static_cast<Word>(plan.node(step.value).components);

        if (isRampStoreIndex(index, width))
        {
            step.ramp = true;
            step.rampScale = index.scale;
        }
    }

private:
    // Lane l writes elements l * scale onwards, so a scale of at least the width
    // keeps lanes apart, and the batch must not wrap into its own first element.
    bool isRampStoreIndex(const LaneForm& index, Word width) const
    {
        return index.linear && index.scale >= width
               && calculateRampSpan(index.scale) + width <= wordRange;
    }

    // The last real lane's element less lane 0's, before any wrap.
    std::uint64_t calculateRampSpan(Word scale) const
    {
        return static_cast<std::uint64_t>(plan.batchLanes() - 1) * scale;
    }

    // A batch is a run of x groups within one (y, z) row, so the thread id's x
    // stays lane-linear across it while the local and group ids' x do not.
    LaneForm calculateIdForm(ExprKind kind, int index) const
    {
        auto shape = plan.groupShape();

        if (shape.y != 1 || shape.z != 1)
            return {};

        if (!isXAxis(index))
            return getLaneInvariantForm();

        if (kind == ExprKind::LocalId && plan.groupsPerBatch() > 1)
            return {};

        return {true, false, 1u, 0u};
    }

    LaneForm calculateGroupIdForm(int index) const
    {
        if (plan.groupsPerBatch() > 1 && isXAxis(index))
            return {};

        return getLaneInvariantForm();
    }

    LaneForm calculateLeafForm(const Expr& expr) const
    {
        switch (expr.kind)
        {
            case ExprKind::Constant:
                return getConstantForm(PlanBuilder::getConstantWord(expr));
            case ExprKind::Uniform:
            case ExprKind::GridExtent:
                return getLaneInvariantForm();
            case ExprKind::GroupId:
                return calculateGroupIdForm(expr.index);
            case ExprKind::ThreadId:
            case ExprKind::LocalId:
                return calculateIdForm(expr.kind, expr.index);
            default:
                return {};
        }
    }

    LaneForm getArgumentForm(int id, const Plan::Node& node, int which) const
    {
        auto argument = plan.argument(node, which);
        return argument < id ? forms[argument] : LaneForm {};
    }

    bool hasScalarArguments(const Plan::Node& node) const
    {
        for (auto which = 0; which < node.argCount; ++which)
            if (plan.node(plan.argument(node, which)).components != 1)
                return false;

        return true;
    }

    LaneForm calculateNodeForm(int id) const
    {
        const auto& node = plan.node(id);

        if (!node.used || node.components != 1)
            return {};

        if (node.op == Op::Leaf)
            return calculateLeafForm(graph.expr(id));

        if (!hasScalarArguments(node))
            return {};

        return calculateOperationForm(id, node);
    }

    LaneForm calculateOperationForm(int id, const Plan::Node& node) const
    {
        auto first = [&] { return getArgumentForm(id, node, 0); };
        auto second = [&] { return getArgumentForm(id, node, 1); };

        switch (node.op)
        {
            case Op::CopyBits:
                return first();
            case Op::AddI:
                return calculateSumForm(first(), second(), 1u);
            case Op::SubI:
                return calculateSumForm(first(), second(), ~0u);
            case Op::MulI:
                return calculateProductForm(first(), second());
            case Op::Shl:
                return calculateShiftForm(first(), second());
            default:
                return {};
        }
    }

    const Plan& plan;
    const ShaderGraph& graph;
    Vector<LaneForm> forms;
};
} // namespace

bool PlanBuilder::usesAtomics() const
{
    auto isAtomicAdd = [](const Plan::Step& step)
    { return step.kind == StatementKind::AtomicAdd; };

    auto isAtomicLoad = [](const Plan::Node& node)
    { return node.used && node.op == Op::AtomicLoad; };

    return std::ranges::any_of(plan.steps, isAtomicAdd)
           || std::ranges::any_of(plan.nodes, isAtomicLoad);
}

// Atomics included: each statement finishing across the batch before the next
// is observable through them, and a batch of one group keeps it so.
bool PlanBuilder::needsGroupPerBatch() const
{
    return graph.usesBarrier() || graph.sharedArrays().size() > 0
           || graph.usesGroupReduction() || graph.usesSimdReduction()
           || graph.usesSimdGroups() || usesAtomics();
}

void PlanBuilder::chooseBatchWidth()
{
    auto groups = std::int64_t {1};

    if (!needsGroupPerBatch())
    {
        auto mostGroups = std::max<std::int64_t>(
            1, std::min<std::int64_t>(maxLanes / plan.laneCount, groupLimit));
        groups = std::clamp<std::int64_t>(
            std::int64_t {options.targetBatchLanes} / plan.laneCount, 1, mostGroups);
    }

    plan.groupsInBatch = static_cast<int>(groups);
    plan.stride = static_cast<int>(roundUp(plan.batchLanes(), laneAlignment));
}

void PlanBuilder::markRamps()
{
    const auto forms = LaneFormAnalysis {plan, graph};

    for (auto id = 0; id < graph.nodeCount(); ++id)
    {
        if (plan.nodes[id].used)
        {
            forms.markRampRead(plan.nodes[id], id);
            forms.markInvariantDivisor(plan.nodes[id], id);
        }
    }

    for (auto& step: plan.steps)
        forms.markRampStore(step);
}

std::uint32_t PlanBuilder::allocate(int components)
{
    auto offset = cursor;
    cursor +=
        static_cast<std::size_t>(components) * static_cast<std::size_t>(plan.stride);
    return static_cast<std::uint32_t>(offset);
}

std::uint32_t PlanBuilder::reuseOrAllocate(Vector<std::uint32_t>& freeScratch,
                                           int components)
{
    if (freeScratch.empty())
        return allocate(components);

    auto scratch = freeScratch.back();
    freeScratch.pop_back();
    return scratch;
}

void PlanBuilder::layOutShared()
{
    plan.sharedOffset = static_cast<std::uint32_t>(cursor);

    for (const auto& shared: graph.sharedArrays())
    {
        auto layout = Plan::SharedLayout {};
        layout.elements = shared.elements;
        layout.components = componentCount(shared.elementType);
        layout.storage = static_cast<std::uint32_t>(cursor);
        cursor += static_cast<std::size_t>(layout.elements)
                  * static_cast<std::size_t>(layout.components);
        plan.sharedLayouts.add(layout);
    }

    auto words = cursor - plan.sharedOffset;
    plan.sharedCount = words <= maxWords ? static_cast<int>(words) : 0;
}

void PlanBuilder::layOutFragments()
{
    plan.fragmentOffset = static_cast<std::uint32_t>(cursor);

    auto words = static_cast<std::size_t>(graph.simdMatrixCount())
                 * static_cast<std::size_t>(plan.simdGroupCount())
                 * static_cast<std::size_t>(Plan::fragmentElements);

    cursor += words;
    plan.fragmentCount = words <= maxWords ? static_cast<int>(words) : 0;
}

void PlanBuilder::layOutNodes()
{
    for (auto id = 0; id < graph.nodeCount(); ++id)
        if (hasOwnScratch(id) && !isPooled(id))
            plan.nodes[id].scratch = allocate(plan.nodes[id].components);

    colourPooledNodes();
}

void PlanBuilder::layOutVariables()
{
    for (auto type: graph.variables())
    {
        auto variable = Plan::Variable {};
        variable.components = componentCount(type);
        variable.storage = allocate(variable.components);
        plan.variableLayouts.add(variable);
    }

    for (auto id = 0; id < graph.nodeCount(); ++id)
    {
        const auto& expr = graph.expr(id);

        if (plan.nodes[id].used && expr.kind == ExprKind::VarRead)
            plan.nodes[id].scratch = plan.variableLayouts[expr.index].storage;
    }
}

void PlanBuilder::layOutFixedRows()
{
    if (graph.usesGroupReduction())
        plan.reductionOffset = allocate(1);

    plan.maskOffset = allocate(plan.maskFrameCount());
    plan.localOffset = allocate(axisCount);
    plan.realLaneOffset = allocate(1);
    plan.batchXOffset = plan.localCoordinates(0);

    if (plan.groupsInBatch > 1)
    {
        plan.groupInBatchOffset = allocate(1);
        plan.batchXOffset = allocate(1);
    }

    for (auto id = 0; id < graph.nodeCount(); ++id)
    {
        const auto& expr = graph.expr(id);
        auto axis = expr.index == allComponents ? 0 : expr.index;

        if (plan.nodes[id].used && expr.kind == ExprKind::LocalId)
            plan.nodes[id].scratch = plan.localCoordinates(axis);
    }
}

void PlanBuilder::layOut()
{
    layOutNodes();
    layOutVariables();

    for (auto& layout: plan.arrayLayouts)
        if (layout.used)
            layout.storage = allocate(layout.components * layout.elementCount);

    layOutShared();
    layOutFragments();
    layOutFixedRows();
    plan.wordCount = cursor;

    if (cursor > maxWords)
        fail(
            "the kernel needs {} words of scratch; the CPU executor addresses at most {}",
            cursor,
            maxWords);
}

bool PlanBuilder::hasOwnScratch(int id) const
{
    return plan.nodes[id].used && ownsScratch(graph.expr(id).kind);
}

// A node evaluated in a statement's schedule, whose scratch is free again once
// its last use has read it.
bool PlanBuilder::isPooled(int id) const
{
    return hasOwnScratch(id) && plan.nodes[id].op != Op::Leaf
           && levels[id] == PlanLevel::Lane;
}

// The commit time of the last step a loop's body holds, or -1 for a step that
// is not a loop.
int PlanBuilder::getLoopEnd(int step) const
{
    if (plan.steps[step].kind != StatementKind::Loop)
        return -1;

    return getCommitTime(plan.steps[subtreeLasts[step]].scheduleEnd);
}

// A value defined outside a loop and read inside it is read again on the next
// iteration, so it lives to the end of every loop between the two.
int PlanBuilder::calculateLoopExtent(int site, int definedAt) const
{
    if (site < 0 || definedAt == unhoisted)
        return -1;

    auto end = getLoopEnd(site);
    auto definedIn = definedAt >= 0 ? stepBlocks[definedAt] : plan.rootBlock();

    for (auto block = stepBlocks[site]; block != definedIn;)
    {
        auto owner = blockOwners[block];

        if (owner < 0 || owner == definedAt)
            break;

        end = std::max(end, getLoopEnd(owner));
        block = stepBlocks[owner];
    }

    return end;
}

void PlanBuilder::notePooledUse(int id, int time, int site)
{
    if (id < 0 || !isPooled(id))
        return;

    auto end = time;

    if (lastDefinitions[id] != site)
        end = std::max(end, calculateLoopExtent(site, lastDefinitions[id]));

    lastUses[id] = std::max(lastUses[id], end);
}

void PlanBuilder::noteScheduled(int site, const Plan::Range& range)
{
    for (auto position = range.begin; position < range.end; ++position)
    {
        auto id = plan.scheduleList[position];
        const auto& node = plan.nodes[id];
        auto time = 2 * position;

        for (auto which = 0; which < node.argCount; ++which)
            notePooledUse(plan.argument(node, which), time, site);

        if (!isPooled(id))
            continue;

        if (definitions[id] < 0)
            definitions[id] = time;

        lastUses[id] = std::max(lastUses[id], time);
        lastDefinitions[id] = site;
    }
}

void PlanBuilder::noteLiveRanges()
{
    definitions.resize(graph.nodeCount(), -1);
    lastUses.resize(graph.nodeCount(), -1);
    lastDefinitions.resize(graph.nodeCount(), unhoisted);

    for (const auto& layout: plan.arrayLayouts)
        if (layout.used)
            noteArrayLiveRanges(layout);

    for (auto stepId = 0; stepId < plan.steps.size(); ++stepId)
        noteStepLiveRanges(stepId);
}

void PlanBuilder::noteArrayLiveRanges(const Plan::ArrayLayout& layout)
{
    noteScheduled(arraySite, layout.schedule);

    for (auto element = 0; element < layout.elementCount; ++element)
        notePooledUse(plan.arrayElement(layout, element),
                      getCommitTime(layout.schedule.end),
                      arraySite);
}

void PlanBuilder::noteStepLiveRanges(int stepId)
{
    const auto& step = plan.steps[stepId];
    noteScheduled(stepId, {step.scheduleBegin, step.scheduleEnd});

    for (auto root: pending[stepId].roots)
        notePooledUse(root, getCommitTime(step.scheduleEnd), stepId);
}

// Greedy interval colouring in definition order. A slot is freed only once a
// later definition begins, so no node ever writes over an operand it is still
// reading, even one whose last use it is.
void PlanBuilder::colourPooledNodes()
{
    noteLiveRanges();

    auto defined = Vector<int> {};
    auto neverDefined = Vector<int> {};

    for (auto id = 0; id < graph.nodeCount(); ++id)
        if (isPooled(id))
            (definitions[id] >= 0 ? defined : neverDefined).add(id);

    colourDefinedNodes(defined);

    for (auto id: neverDefined)
        plan.nodes[id].scratch = allocate(plan.nodes[id].components);
}

void PlanBuilder::colourDefinedNodes(Vector<int> byDefinition)
{
    auto byLastUse = byDefinition;

    std::sort(byDefinition.begin(),
              byDefinition.end(),
              [this](int a, int b) { return definitions[a] < definitions[b]; });

    std::stable_sort(byLastUse.begin(),
                     byLastUse.end(),
                     [this](int a, int b) { return lastUses[a] < lastUses[b]; });

    auto freeScratch = Array<Vector<std::uint32_t>, mostComponents + 1> {};
    auto released = 0;

    for (auto id: byDefinition)
    {
        auto hasEnded = [&]
        { return lastUses[byLastUse[released]] < definitions[id]; };

        for (; released < byLastUse.size() && hasEnded(); ++released)
        {
            const auto& freed = plan.nodes[byLastUse[released]];
            freeScratch[freed.components].add(freed.scratch);
        }

        auto& node = plan.nodes[id];
        node.scratch =
            reuseOrAllocate(freeScratch[node.components], node.components);
    }
}
} // namespace eacp::GPU::CpuCompute
