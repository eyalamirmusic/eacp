#include "Interpreter.h"

#include <cmath>
#include <cstdint>
#include <limits>

// Each statement evaluates all lanes, then commits only under the frame's mask.
// The group runs in lockstep, so statement order already is a barrier.

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

void storeElements(const Context& context,
                   const Plan::Step& step,
                   const MaskFrame& frame,
                   int width)
{
    evaluateStep(context, step);

    const auto& view = context.slots[static_cast<std::size_t>(step.slot)];

    if (view.count == 0)
        return;

    const auto* indices = context.lanes(context.plan.node(step.index));
    const auto& value = context.plan.node(step.value);
    const auto* mask = frame.mask;
    auto lanes = context.plan.lanes();

    for (auto lane = 0; lane < lanes; ++lane)
    {
        if (mask[lane] == 0)
            continue;

        for (auto component = 0; component < width; ++component)
        {
            auto element = indices[lane] + static_cast<Word>(component);

            if (element < view.count)
                view.store(element, context.lanes(value, component)[lane]);
        }
    }
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
    auto lanes = context.plan.lanes();

    for (auto lane = 0; lane < lanes; ++lane)
    {
        if (frame.mask[lane] == 0 || indices[lane] >= elements)
            continue;

        auto* element = storage + indices[lane] * static_cast<Word>(components);

        for (auto component = 0; component < components; ++component)
            element[component] = context.lanes(value, component)[lane];
    }
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
    auto lanes = context.plan.lanes();

    for (auto lane = 0; lane < lanes; ++lane)
    {
        if (frame.mask[lane] == 0)
            continue;

        auto element = indices[lane];
        previous[lane] =
            element < view.count ? view.atomicAdd(element, values[lane]) : 0u;
    }
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
        if (frame.mask[lane] != 0)
            result[lane] = scratch[lane / width * width];
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
