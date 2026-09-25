#include "Interpreter.h"

#include <cmath>
#include <cstdint>
#include <limits>

// Each statement evaluates all lanes, then commits only under the frame's mask.
// The batch runs in lockstep, so statement order already is a barrier; a batch
// holds more than one group only when the kernel has nothing group-scoped.

namespace eacp::GPU::CpuCompute
{
namespace
{
Word* statementFrame(const Context& context, int depth, int which)
{
    return context.lanes(context.plan.maskFrame(1 + 2 * depth + which));
}

void evaluateStep(const Context& context, const Plan::Step& step)
{
    evaluateRange(context, step.scheduleBegin, step.scheduleEnd);
}

void assignVariable(const Context& context,
                    const Plan::Step& step,
                    const MaskFrame& frame)
{
    evaluateStep(context, step);

    const auto& variable = context.plan.variables()[step.slot];
    const auto& value = context.plan.node(step.value);

    for (auto component = 0; component < variable.components; ++component)
        Lanes::blend(
            context.lanes(variable.storage
                          + static_cast<std::uint32_t>(component * context.stride)),
            context.lanes(value, component),
            frame.mask,
            context.stride);
}

// The active lanes gathered first without a branch, then visited in ascending
// order, so a divergent mask costs no mispredicted branch per lane.
template <typename Visit>
void forEachActiveLane(const Word* mask, int lanes, Visit visit)
{
    constexpr auto chunk = 64;
    auto active = std::array<int, chunk> {};

    for (auto first = 0; first < lanes; first += chunk)
    {
        auto end = first + chunk < lanes ? first + chunk : lanes;
        auto count = 0;

        for (auto lane = first; lane < end; ++lane)
        {
            active[static_cast<std::size_t>(count)] = lane;
            count += mask[lane] != 0 ? 1 : 0;
        }

        for (auto which = 0; which < count; ++which)
            visit(active[static_cast<std::size_t>(which)]);
    }
}

bool isWholeRun(const Word* mask, int count)
{
    auto all = ~Word {0};

    for (auto lane = 0; lane < count; ++lane)
        all &= mask[lane];

    return all != 0;
}

template <int FixedScale>
void storeStrided(
    std::byte* first, const Word* value, const Word* mask, int count, Word scale)
{
    auto step =
        static_cast<std::size_t>(FixedScale > 0 ? FixedScale : scale) * sizeof(Word);

    auto storeLane = [&](int lane)
    {
        std::memcpy(first + static_cast<std::size_t>(lane) * step,
                    value + lane,
                    sizeof(Word));
    };

    if (!isWholeRun(mask, count))
    {
        forEachActiveLane(mask, count, storeLane);
        return;
    }

    for (auto lane = 0; lane < count; ++lane)
        storeLane(lane);
}

void storeRun(const SlotView& view,
              const LaneRun& run,
              const Word* value,
              const Word* mask,
              Word scale)
{
    auto* first = view.data + sizeof(Word) * run.element;
    auto count = run.end - run.begin;
    value += run.begin;
    mask += run.begin;

    switch (scale)
    {
        case 1:
            storeStrided<1>(first, value, mask, count, scale);
            return;
        case 2:
            storeStrided<2>(first, value, mask, count, scale);
            return;
        case 3:
            storeStrided<3>(first, value, mask, count, scale);
            return;
        case 4:
            storeStrided<4>(first, value, mask, count, scale);
            return;
        default:
            storeStrided<0>(first, value, mask, count, scale);
            return;
    }
}

// Every (lane, component) of a ramp store has its own element, so the order the
// lanes commit in cannot show. A masked-out lane's element is never touched:
// another thread may be writing it.
void storeRamp(const Context& context,
               const Plan::Step& step,
               const MaskFrame& frame,
               int width,
               Word scale)
{
    const auto& view = context.slots[static_cast<std::size_t>(step.slot)];
    const auto* indices = context.lanes(context.plan.node(step.index));
    const auto& value = context.plan.node(step.value);

    for (auto component = 0; component < width; ++component)
    {
        auto start = indices[0] + static_cast<Word>(component);
        auto runs =
            RampBounds(start, scale, context.plan.batchLanes()).inRange(view.count);

        for (auto which = 0; which < runs.count; ++which)
            storeRun(view,
                     runs.runs[static_cast<std::size_t>(which)],
                     context.lanes(value, component),
                     frame.mask,
                     scale);
    }
}

void storeElements(const Context& context,
                   const Plan::Step& step,
                   const MaskFrame& frame,
                   int width)
{
    evaluateStep(context, step);

    const auto& view = context.slots[static_cast<std::size_t>(step.slot)];

    if (view.count == 0)
        return;

    if (step.ramp)
    {
        storeRamp(context, step, frame, width, step.rampScale);
        return;
    }

    const auto* indices = context.lanes(context.plan.node(step.index));
    const auto& value = context.plan.node(step.value);

    if (width == 1 && isContiguousRow(indices, context.plan.batchLanes()))
    {
        storeRamp(context, step, frame, width, 1u);
        return;
    }

    forEachActiveLane(
        frame.mask,
        context.plan.batchLanes(),
        [&](int lane)
        {
            for (auto component = 0; component < width; ++component)
            {
                auto element = indices[lane] + static_cast<Word>(component);

                if (element < view.count)
                    view.store(element, context.lanes(value, component)[lane]);
            }
        });
}

void storeShared(const Context& context,
                 const Plan::Step& step,
                 const MaskFrame& frame)
{
    evaluateStep(context, step);

    const auto& shared = context.plan.sharedArrays()[step.slot];
    const auto* indices = context.lanes(context.plan.node(step.index));
    const auto& value = context.plan.node(step.value);
    auto* storage = context.lanes(shared.storage);
    auto elements = static_cast<Word>(shared.elements);
    auto components = shared.components;

    forEachActiveLane(
        frame.mask,
        context.plan.lanes(),
        [&](int lane)
        {
            if (indices[lane] >= elements)
                return;

            auto* element = storage + indices[lane] * static_cast<Word>(components);

            for (auto component = 0; component < components; ++component)
                element[component] = context.lanes(value, component)[lane];
        });
}

void addAtomically(const Context& context,
                   const Plan::Step& step,
                   const MaskFrame& frame)
{
    evaluateStep(context, step);

    const auto& view = context.slots[static_cast<std::size_t>(step.buffer)];
    const auto* indices = context.lanes(context.plan.node(step.index));
    const auto* values = context.lanes(context.plan.node(step.value));
    auto* previous = context.lanes(context.plan.variables()[step.slot].storage);

    forEachActiveLane(frame.mask,
                      context.plan.lanes(),
                      [&](int lane)
                      {
                          auto element = indices[lane];
                          previous[lane] =
                              element < view.count
                                  ? view.atomicAdd(element, values[lane])
                                  : 0u;
                      });
}

template <typename Fold>
void foldHalving(Word* scratch, int first, int width, int end, Fold fold)
{
    auto step = 1;

    while (step * 2 < width)
        step *= 2;

    for (; step > 0; step >>= 1)
        for (auto within = 0;
             within < step && within + step < width && first + within + step < end;
             ++within)
            scratch[first + within] =
                fold(scratch[first + within], scratch[first + within + step]);
}

template <typename Fold>
void foldBlocks(Word* scratch, int lanes, int width, Fold fold)
{
    for (auto first = 0; first < lanes; first += width)
        foldHalving(scratch, first, width, lanes, fold);
}

Word reductionIdentity(GroupReduction reduction, ValueType type)
{
    auto isFloat = type == ValueType::Float;
    auto isSigned = type == ValueType::Int;

    switch (reduction)
    {
        case GroupReduction::Sum:
            return isFloat ? Lanes::toWord(-0.f) : 0u;
        case GroupReduction::Max:
            if (isFloat)
                return Lanes::toWord(-std::numeric_limits<float>::infinity());

            return isSigned ? Lanes::fromSigned(INT32_MIN) : 0u;
        case GroupReduction::Min:
            if (isFloat)
                return Lanes::toWord(std::numeric_limits<float>::infinity());

            return isSigned ? Lanes::fromSigned(INT32_MAX) : UINT32_MAX;
    }

    return 0u;
}

void foldScratch(Word* scratch, int lanes, int width, const Plan::Step& step)
{
    using Lanes::fromSigned;
    using Lanes::toFloat;
    using Lanes::toSigned;
    using Lanes::toWord;

    auto run = [&](auto fold) { foldBlocks(scratch, lanes, width, fold); };

    if (step.type == ValueType::Float)
    {
        switch (step.reduction)
        {
            case GroupReduction::Sum:
                run([](Word a, Word b) { return toWord(toFloat(a) + toFloat(b)); });
                return;
            case GroupReduction::Max:
                run([](Word a, Word b)
                    { return toWord(std::fmax(toFloat(a), toFloat(b))); });
                return;
            case GroupReduction::Min:
                run([](Word a, Word b)
                    { return toWord(std::fmin(toFloat(a), toFloat(b))); });
                return;
        }
    }

    if (step.type == ValueType::Int && step.reduction != GroupReduction::Sum)
    {
        auto isMax = step.reduction == GroupReduction::Max;
        run(
            [isMax](Word a, Word b)
            {
                auto left = toSigned(a);
                auto right = toSigned(b);
                return fromSigned(isMax == (left > right) ? left : right);
            });
        return;
    }

    switch (step.reduction)
    {
        case GroupReduction::Sum:
            run([](Word a, Word b) { return a + b; });
            return;
        case GroupReduction::Max:
            run([](Word a, Word b) { return a > b ? a : b; });
            return;
        case GroupReduction::Min:
            run([](Word a, Word b) { return a < b ? a : b; });
            return;
    }
}

int reductionWidth(const Plan& plan, ReductionScope scope)
{
    auto lanes = plan.lanes();

    if (scope == ReductionScope::Group || lanes < simdGroupWidth)
        return lanes;

    return simdGroupWidth;
}

void reduceGroup(const Context& context,
                 const Plan::Step& step,
                 const MaskFrame& frame)
{
    evaluateStep(context, step);

    const auto& plan = context.plan;
    const auto* value = context.lanes(plan.node(step.value));
    auto* scratch = context.lanes(plan.reductionScratch());
    auto* result = context.lanes(plan.variables()[step.slot].storage);
    auto identity = reductionIdentity(step.reduction, step.type);
    auto lanes = plan.lanes();
    auto width = reductionWidth(plan, step.scope);

    for (auto lane = 0; lane < lanes; ++lane)
        scratch[lane] = frame.mask[lane] != 0 ? value[lane] : identity;

    foldScratch(scratch, lanes, width, step);

    for (auto lane = 0; lane < lanes; ++lane)
    {
        auto mask = frame.mask[lane];
        auto folded = scratch[lane / width * width];
        result[lane] = (folded & mask) | (result[lane] & ~mask);
    }
}

void leaveLoop(MaskFrame& frame, const LoopFrame* loop, bool breaking, int stride)
{
    if (&frame != loop->iteration)
    {
        for (auto* outer = frame.parent; outer != nullptr; outer = outer->parent)
        {
            Lanes::clearWhere(outer->mask, frame.mask, stride);

            if (outer == loop->iteration)
                break;
        }
    }

    if (breaking)
        Lanes::clearWhere(loop->live, frame.mask, stride);

    Lanes::fill(frame.mask, 0u, stride);
}

void runBranch(const Context& context,
               const Plan::Step& step,
               MaskFrame& frame,
               int depth,
               const LoopFrame* loop)
{
    evaluateStep(context, step);

    const auto* condition = context.lanes(context.plan.node(step.value));
    auto stride = context.stride;

    auto thenFrame = MaskFrame {statementFrame(context, depth, 0), &frame};
    auto elseFrame = MaskFrame {statementFrame(context, depth, 1), &frame};

    Lanes::intersect(thenFrame.mask, frame.mask, condition, stride);
    Lanes::intersectComplement(elseFrame.mask, frame.mask, condition, stride);

    if (Lanes::anyActive(thenFrame.mask, stride))
        runBlock(context, step.body, thenFrame, depth + 1, loop);

    if (step.elseBody >= 0 && Lanes::anyActive(elseFrame.mask, stride))
        runBlock(context, step.elseBody, elseFrame, depth + 1, loop);
}

void runLoop(const Context& context,
             const Plan::Step& step,
             MaskFrame& frame,
             int depth)
{
    auto stride = context.stride;
    auto* live = statementFrame(context, depth, 0);
    auto iteration = MaskFrame {statementFrame(context, depth, 1), &frame};
    auto loop = LoopFrame {&iteration, live};
    const auto* condition = context.lanes(context.plan.node(step.value));

    Lanes::copy(live, frame.mask, stride);

    for (;;)
    {
        evaluateStep(context, step);
        Lanes::intersect(live, live, condition, stride);

        if (!Lanes::anyActive(live, stride))
            return;

        Lanes::copy(iteration.mask, live, stride);
        runBlock(context, step.body, iteration, depth + 1, &loop);
    }
}
} // namespace

void runBlock(const Context& context,
              int block,
              MaskFrame& frame,
              int depth,
              const LoopFrame* loop)
{
    const auto& plan = context.plan;
    const auto& range = plan.block(block);

    for (auto position = range.begin; position < range.end; ++position)
    {
        const auto& step = plan.step(plan.blockStep(position));

        switch (step.kind)
        {
            case StatementKind::Declare:
            case StatementKind::Assign:
                assignVariable(context, step, frame);
                break;

            case StatementKind::If:
                runBranch(context, step, frame, depth, loop);

                if (step.bodiesJumpOut
                    && !Lanes::anyActive(frame.mask, context.stride))
                    return;

                break;

            case StatementKind::Loop:
                runLoop(context, step, frame, depth);
                break;

            case StatementKind::Break:
                leaveLoop(frame, loop, true, context.stride);
                return;

            case StatementKind::Continue:
                leaveLoop(frame, loop, false, context.stride);
                return;

            case StatementKind::Store:
                storeElements(context, step, frame, 1);
                break;

            case StatementKind::VectorStore:
                storeElements(
                    context, step, frame, plan.node(step.value).components);
                break;

            case StatementKind::SharedStore:
                storeShared(context, step, frame);
                break;

            case StatementKind::GroupReduce:
                reduceGroup(context, step, frame);
                break;

            case StatementKind::AtomicAdd:
                addAtomically(context, step, frame);
                break;

            case StatementKind::SimdMatrixFill:
                fillSimdMatrix(context, step, frame);
                break;

            case StatementKind::SimdMatrixLoad:
                loadSimdMatrix(context, step, frame);
                break;

            case StatementKind::SimdMatrixStore:
                storeSimdMatrix(context, step, frame);
                break;

            case StatementKind::SimdMatrixMultiplyAdd:
                multiplyAddSimdMatrix(context, step, frame);
                break;

            case StatementKind::Barrier:
            default:
                break;
        }
    }
}
} // namespace eacp::GPU::CpuCompute
