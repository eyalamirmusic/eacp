#include "Interpreter.h"

#include <climits>

namespace eacp::GPU::CpuCompute
{
namespace
{
using namespace Lanes;

void evaluateConstruct(const Context& context, const Plan::Node& node)
{
    auto written = 0;

    for (auto which = 0; which < node.argCount; ++which)
    {
        const auto& argument = context.argumentNode(node, which);

        for (auto component = 0; component < argument.components; ++component)
            copy(context.lanes(node, written++),
                 context.lanes(argument, component),
                 context.stride);
    }
}

void evaluateSwizzle(const Context& context, const Plan::Node& node)
{
    const auto& source = context.argumentNode(node, 0);

    for (auto component = 0; component < node.components; ++component)
        copy(
            context.lanes(node, component),
            context.lanes(source, node.swizzle[static_cast<std::size_t>(component)]),
            context.stride);
}

void evaluateSelect(const Context& context, const Plan::Node& node)
{
    const auto* condition = context.operand(node, 0, 0);

    for (auto component = 0; component < node.components; ++component)
        select(context.lanes(node, component),
               condition,
               context.operand(node, 1, component),
               context.operand(node, 2, component),
               context.stride);
}

template <typename Function>
void evaluateMapWords(const Context& context,
                      const Plan::Node& node,
                      Function function)
{
    for (auto component = 0; component < node.components; ++component)
        mapWords(context.lanes(node, component),
                 context.operand(node, 0, component),
                 context.stride,
                 function);
}

template <typename Function>
void evaluateMapFloats(const Context& context,
                       const Plan::Node& node,
                       Function function)
{
    for (auto component = 0; component < node.components; ++component)
        mapFloats(context.lanes(node, component),
                  context.operand(node, 0, component),
                  context.stride,
                  function);
}

template <typename Function>
void evaluateZipFloats(const Context& context,
                       const Plan::Node& node,
                       Function function)
{
    for (auto component = 0; component < node.components; ++component)
        zipFloats(context.lanes(node, component),
                  context.operand(node, 0, component),
                  context.operand(node, 1, component),
                  context.stride,
                  function);
}

template <typename Function>
void evaluateZipWords(const Context& context,
                      const Plan::Node& node,
                      Function function)
{
    for (auto component = 0; component < node.components; ++component)
        zipWords(context.lanes(node, component),
                 context.operand(node, 0, component),
                 context.operand(node, 1, component),
                 context.stride,
                 function);
}

template <typename Function>
void evaluateZipSigned(const Context& context,
                       const Plan::Node& node,
                       Function function)
{
    for (auto component = 0; component < node.components; ++component)
        zipSigned(context.lanes(node, component),
                  context.operand(node, 0, component),
                  context.operand(node, 1, component),
                  context.stride,
                  function);
}

template <typename Compare>
void evaluateRelation(const Context& context,
                      const Plan::Node& node,
                      Compare compare)
{
    for (auto component = 0; component < node.components; ++component)
        compare(context.lanes(node, component),
                context.operand(node, 0, component),
                context.operand(node, 1, component),
                context.stride);
}

template <typename Relate>
void compareBy(const Context& context, const Plan::Node& node, Relate relate)
{
    evaluateRelation(context,
                     node,
                     [&](Word* out, const Word* a, const Word* b, int count)
                     { relate(out, a, b, count); });
}

void evaluateCompareFloats(const Context& context, const Plan::Node& node)
{
    auto run = [&](auto relation)
    {
        compareBy(context,
                  node,
                  [&](Word* out, const Word* a, const Word* b, int count)
                  { compareFloats(out, a, b, count, relation); });
    };

    switch ((Relation) node.sub)
    {
        case Relation::Less:
            run([](float a, float b) { return a < b; });
            return;
        case Relation::LessEqual:
            run([](float a, float b) { return a <= b; });
            return;
        case Relation::Greater:
            run([](float a, float b) { return a > b; });
            return;
        case Relation::GreaterEqual:
            run([](float a, float b) { return a >= b; });
            return;
        case Relation::Equal:
            run([](float a, float b) { return a == b; });
            return;
        case Relation::NotEqual:
            run([](float a, float b) { return a != b; });
            return;
    }
}

void evaluateCompareUnsigned(const Context& context, const Plan::Node& node)
{
    auto run = [&](auto relation)
    {
        compareBy(context,
                  node,
                  [&](Word* out, const Word* a, const Word* b, int count)
                  { compareWords(out, a, b, count, relation); });
    };

    switch ((Relation) node.sub)
    {
        case Relation::Less:
            run([](Word a, Word b) { return a < b; });
            return;
        case Relation::LessEqual:
            run([](Word a, Word b) { return a <= b; });
            return;
        case Relation::Greater:
            run([](Word a, Word b) { return a > b; });
            return;
        case Relation::GreaterEqual:
            run([](Word a, Word b) { return a >= b; });
            return;
        case Relation::Equal:
            run([](Word a, Word b) { return a == b; });
            return;
        case Relation::NotEqual:
            run([](Word a, Word b) { return a != b; });
            return;
    }
}

void evaluateCompareSigned(const Context& context, const Plan::Node& node)
{
    auto run = [&](auto relation)
    {
        compareBy(context,
                  node,
                  [&](Word* out, const Word* a, const Word* b, int count)
                  { compareSigned(out, a, b, count, relation); });
    };

    switch ((Relation) node.sub)
    {
        case Relation::Less:
            run([](std::int32_t a, std::int32_t b) { return a < b; });
            return;
        case Relation::LessEqual:
            run([](std::int32_t a, std::int32_t b) { return a <= b; });
            return;
        case Relation::Greater:
            run([](std::int32_t a, std::int32_t b) { return a > b; });
            return;
        case Relation::GreaterEqual:
            run([](std::int32_t a, std::int32_t b) { return a >= b; });
            return;
        case Relation::Equal:
            run([](std::int32_t a, std::int32_t b) { return a == b; });
            return;
        case Relation::NotEqual:
            run([](std::int32_t a, std::int32_t b) { return a != b; });
            return;
    }
}

std::int32_t divideSigned(std::int32_t a, std::int32_t b)
{
    if (b == 0)
        return 0;

    return a == INT_MIN && b == -1 ? INT_MIN : a / b;
}

std::int32_t remainderSigned(std::int32_t a, std::int32_t b)
{
    if (b == 0)
        return 0;

    return a == INT_MIN && b == -1 ? 0 : a % b;
}

// a / d through d's reciprocal in double: for any 32-bit a the product is within
// a / d * 2^-52 of the quotient, less than the 1 / d that separates it from the
// next integer, so truncating it is the quotient or, at an exact multiple, one
// below - which the remainder shows and one step corrects.
struct InvariantDivisor
{
    explicit InvariantDivisor(Word divisorToUse)
        : divisor(divisorToUse)
        , reciprocal(1.0 / static_cast<double>(divisorToUse))
    {
    }

    Word quotient(Word a, Word& remainder) const
    {
        auto estimate = static_cast<Word>(static_cast<double>(a) * reciprocal);
        auto rest = a - estimate * divisor;
        auto isShort = rest >= divisor;
        remainder = isShort ? rest - divisor : rest;
        return isShort ? estimate + 1u : estimate;
    }

    Word divisor;
    double reciprocal;
};

void evaluateInvariantDivision(const Context& context,
                               const Plan::Node& node,
                               bool wantsRemainder)
{
    auto stride = context.stride;
    auto divisor = context.operand(node, 1, 0)[0];

    for (auto component = 0; component < node.components; ++component)
    {
        auto* out = context.lanes(node, component);

        if (divisor == 0)
        {
            fill(out, 0u, stride);
            continue;
        }

        const auto* a = context.operand(node, 0, component);
        auto division = InvariantDivisor(divisor);

        for (auto lane = 0; lane < stride; ++lane)
        {
            auto remainder = Word {};
            auto quotient = division.quotient(a[lane], remainder);
            out[lane] = wantsRemainder ? remainder : quotient;
        }
    }
}

using OperandLanes = std::array<const Word*, 16>;

OperandLanes operandLanes(const Context& context,
                          const Plan::Node& node,
                          int which,
                          int count)
{
    auto lanes = OperandLanes {};

    for (auto component = 0; component < count; ++component)
        lanes[static_cast<std::size_t>(component)] =
            context.operand(node, which, component);

    return lanes;
}

void evaluateMatrixVector(const Context& context, const Plan::Node& node)
{
    auto order = static_cast<int>(node.order);
    auto matrix = operandLanes(context, node, 0, order * order);
    auto vector = operandLanes(context, node, 1, order);
    auto stride = context.stride;

    for (auto row = 0; row < order; ++row)
    {
        auto* out = context.lanes(node, row);

        for (auto lane = 0; lane < stride; ++lane)
        {
            auto sum = 0.f;

            for (auto column = 0; column < order; ++column)
            {
                auto m = toFloat(matrix[column * order + row][lane]);
                auto v = toFloat(vector[column][lane]);
                sum = column == 0 ? m * v : sum + m * v;
            }

            out[lane] = toWord(sum);
        }
    }
}

void evaluateVectorMatrix(const Context& context, const Plan::Node& node)
{
    auto order = static_cast<int>(node.order);
    auto vector = operandLanes(context, node, 0, order);
    auto matrix = operandLanes(context, node, 1, order * order);
    auto stride = context.stride;

    for (auto column = 0; column < order; ++column)
    {
        auto* out = context.lanes(node, column);

        for (auto lane = 0; lane < stride; ++lane)
        {
            auto sum = 0.f;

            for (auto row = 0; row < order; ++row)
            {
                auto v = toFloat(vector[row][lane]);
                auto m = toFloat(matrix[column * order + row][lane]);
                sum = row == 0 ? v * m : sum + v * m;
            }

            out[lane] = toWord(sum);
        }
    }
}

void evaluateMatrixMatrix(const Context& context, const Plan::Node& node)
{
    auto order = static_cast<int>(node.order);
    auto leftMatrix = operandLanes(context, node, 0, order * order);
    auto rightMatrix = operandLanes(context, node, 1, order * order);
    auto stride = context.stride;

    for (auto column = 0; column < order; ++column)
    {
        for (auto row = 0; row < order; ++row)
        {
            auto* out = context.lanes(node, column * order + row);

            for (auto lane = 0; lane < stride; ++lane)
            {
                auto sum = 0.f;

                for (auto k = 0; k < order; ++k)
                {
                    auto left = toFloat(leftMatrix[k * order + row][lane]);
                    auto right = toFloat(rightMatrix[column * order + k][lane]);
                    sum = k == 0 ? left * right : sum + left * right;
                }

                out[lane] = toWord(sum);
            }
        }
    }
}

void readElements(
    Word* out, const Word* indices, Word offset, const SlotView& view, int count)
{
    auto size = view.count;

    if (size == 0)
    {
        Lanes::fill(out, 0u, count);
        return;
    }

    for (auto lane = 0; lane < count; ++lane)
    {
        auto element = indices[lane] + offset;
        auto inRange = element < size;
        auto value = view.load(inRange ? element : 0u);
        out[lane] = inRange ? value : 0u;
    }
}

template <int FixedScale>
void copyStrided(Word* out, const std::byte* first, int count, Word scale)
{
    auto step =
        static_cast<std::size_t>(FixedScale > 0 ? FixedScale : scale) * sizeof(Word);

    for (auto lane = 0; lane < count; ++lane)
        std::memcpy(
            out + lane, first + static_cast<std::size_t>(lane) * step, sizeof(Word));
}

void copyRun(Word* out, const SlotView& view, const LaneRun& run, Word scale)
{
    const auto* first = view.data + sizeof(Word) * run.element;
    auto count = run.end - run.begin;
    out += run.begin;

    switch (scale)
    {
        case 0:
            Lanes::fill(out, view.load(run.element), count);
            return;
        case 1:
            copyStrided<1>(out, first, count, scale);
            return;
        case 2:
            copyStrided<2>(out, first, count, scale);
            return;
        case 3:
            copyStrided<3>(out, first, count, scale);
            return;
        case 4:
            copyStrided<4>(out, first, count, scale);
            return;
        default:
            copyStrided<0>(out, first, count, scale);
            return;
    }
}

void readRamp(Word* out, Word start, Word scale, const SlotView& view, int lanes)
{
    auto runs = RampBounds(start, scale, lanes).inRange(view.count);
    auto zeroFrom = 0;

    for (auto which = 0; which < runs.count; ++which)
    {
        const auto& run = runs.runs[static_cast<std::size_t>(which)];
        Lanes::fill(out + zeroFrom, 0u, run.begin - zeroFrom);
        copyRun(out, view, run, scale);
        zeroFrom = run.end;
    }

    Lanes::fill(out + zeroFrom, 0u, lanes - zeroFrom);
}

void evaluateBufferRead(const Context& context, const Plan::Node& node)
{
    const auto& view = context.slots[static_cast<std::size_t>(node.immediate)];
    const auto* indices = context.operand(node, 0, 0);
    auto lanes = context.plan.batchLanes();
    auto isRamp = node.ramp || isContiguousRow(indices, lanes);
    auto scale = node.ramp ? node.rampScale : 1u;
    auto rampLanes = isRamp ? lanes : 0;
    auto padding = context.stride - rampLanes;

    for (auto component = 0; component < node.components; ++component)
    {
        auto* out = context.lanes(node, component);
        auto offset = static_cast<Word>(component);

        if (isRamp)
            readRamp(out, indices[0] + offset, scale, view, rampLanes);

        readElements(out + rampLanes, indices + rampLanes, offset, view, padding);
    }
}

void evaluateArrayRead(const Context& context, const Plan::Node& node)
{
    const auto& array = context.plan.arrays()[node.immediate];
    const auto* indices = context.operand(node, 0, 0);
    const auto* storage = context.lanes(array.storage);
    auto stride = context.stride;
    auto elements = array.elementCount;

    for (auto component = 0; component < node.components; ++component)
    {
        auto* out = context.lanes(node, component);

        for (auto lane = 0; lane < stride; ++lane)
        {
            auto index = toSigned(indices[lane]);
            auto inRange = index >= 0 && index < elements;
            auto element = inRange ? index : 0;
            auto at = (element * array.components + component) * stride + lane;
            auto value = elements > 0 ? storage[at] : 0u;
            out[lane] = inRange ? value : 0u;
        }
    }
}

void evaluateSharedRead(const Context& context, const Plan::Node& node)
{
    const auto& shared = context.plan.sharedArrays()[node.immediate];
    const auto* indices = context.operand(node, 0, 0);
    const auto* storage = context.lanes(shared.storage);
    auto elements = static_cast<Word>(shared.elements);
    auto components = static_cast<Word>(shared.components);
    auto stride = context.stride;

    for (auto component = 0; component < node.components; ++component)
    {
        auto* out = context.lanes(node, component);

        if (elements == 0)
        {
            fill(out, 0u, stride);
            continue;
        }

        for (auto lane = 0; lane < stride; ++lane)
        {
            auto inRange = indices[lane] < elements;
            auto element = inRange ? indices[lane] : 0u;
            auto value =
                storage[element * components + static_cast<Word>(component)];
            out[lane] = inRange ? value : 0u;
        }
    }
}

void evaluateAtomicLoad(const Context& context, const Plan::Node& node)
{
    const auto& view = context.slots[static_cast<std::size_t>(node.immediate)];
    const auto* indices = context.operand(node, 0, 0);
    auto* out = context.lanes(node);
    auto stride = context.stride;

    for (auto lane = 0; lane < stride; ++lane)
        out[lane] = indices[lane] < view.count ? view.atomicLoad(indices[lane]) : 0u;
}

void evaluateNode(const Context& context, const Plan::Node& node)
{
    switch (node.op)
    {
        case Op::Leaf:
            return;

        case Op::Construct:
            evaluateConstruct(context, node);
            return;

        case Op::Swizzle:
            evaluateSwizzle(context, node);
            return;

        case Op::Select:
            evaluateSelect(context, node);
            return;

        case Op::NegF:
            evaluateMapFloats(context, node, [](float a) { return -a; });
            return;

        case Op::NegI:
            evaluateMapWords(context, node, [](Word a) { return Word {0} - a; });
            return;

        case Op::BitNot:
            evaluateMapWords(context, node, [](Word a) { return ~a; });
            return;

        case Op::AddF:
            evaluateZipFloats(context, node, [](float a, float b) { return a + b; });
            return;

        case Op::SubF:
            evaluateZipFloats(context, node, [](float a, float b) { return a - b; });
            return;

        case Op::MulF:
            evaluateZipFloats(context, node, [](float a, float b) { return a * b; });
            return;

        case Op::DivF:
            evaluateZipFloats(context, node, [](float a, float b) { return a / b; });
            return;

        case Op::AddI:
            evaluateZipWords(context, node, [](Word a, Word b) { return a + b; });
            return;

        case Op::SubI:
            evaluateZipWords(context, node, [](Word a, Word b) { return a - b; });
            return;

        case Op::MulI:
            evaluateZipWords(context, node, [](Word a, Word b) { return a * b; });
            return;

        case Op::DivU:
            if (node.invariantDivisor)
                evaluateInvariantDivision(context, node, false);
            else
                evaluateZipWords(context,
                                 node,
                                 [](Word a, Word b) { return b != 0 ? a / b : 0u; });
            return;

        case Op::RemU:
            if (node.invariantDivisor)
                evaluateInvariantDivision(context, node, true);
            else
                evaluateZipWords(context,
                                 node,
                                 [](Word a, Word b) { return b != 0 ? a % b : 0u; });
            return;

        case Op::DivS:
            evaluateZipSigned(context, node, divideSigned);
            return;

        case Op::RemS:
            evaluateZipSigned(context, node, remainderSigned);
            return;

        case Op::And:
            evaluateZipWords(context, node, [](Word a, Word b) { return a & b; });
            return;

        case Op::Or:
            evaluateZipWords(context, node, [](Word a, Word b) { return a | b; });
            return;

        case Op::Xor:
            evaluateZipWords(context, node, [](Word a, Word b) { return a ^ b; });
            return;

        case Op::EqMask:
            evaluateZipWords(context, node, [](Word a, Word b) { return ~(a ^ b); });
            return;

        case Op::Shl:
            evaluateZipWords(
                context, node, [](Word a, Word b) { return a << (b & 31u); });
            return;

        case Op::ShrU:
            evaluateZipWords(
                context, node, [](Word a, Word b) { return a >> (b & 31u); });
            return;

        case Op::ShrS:
            evaluateZipWords(context,
                             node,
                             [](Word a, Word b)
                             { return fromSigned(toSigned(a) >> (b & 31u)); });
            return;

        case Op::CmpF:
            evaluateCompareFloats(context, node);
            return;

        case Op::CmpU:
            evaluateCompareUnsigned(context, node);
            return;

        case Op::CmpS:
            evaluateCompareSigned(context, node);
            return;

        case Op::MatVec:
            evaluateMatrixVector(context, node);
            return;

        case Op::VecMat:
            evaluateVectorMatrix(context, node);
            return;

        case Op::MatMat:
            evaluateMatrixMatrix(context, node);
            return;

        case Op::BufferRead:
        case Op::BufferVectorRead:
            evaluateBufferRead(context, node);
            return;

        case Op::ArrayRead:
            evaluateArrayRead(context, node);
            return;

        case Op::SharedRead:
            evaluateSharedRead(context, node);
            return;

        case Op::AtomicLoad:
            evaluateAtomicLoad(context, node);
            return;

        default:
            evaluateCall(context, node);
            return;
    }
}
} // namespace

void evaluateRange(const Context& context, int begin, int end)
{
    const auto& schedule = context.plan.schedule();

    for (auto position = begin; position < end; ++position)
        evaluateNode(context, context.plan.node(schedule[position]));
}
} // namespace eacp::GPU::CpuCompute
