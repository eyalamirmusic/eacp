#include "Interpreter.h"

// Each statement evaluates all lanes, then commits only under the frame's mask.

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

            default:
                break;
        }
    }
}
} // namespace eacp::GPU::CpuCompute
