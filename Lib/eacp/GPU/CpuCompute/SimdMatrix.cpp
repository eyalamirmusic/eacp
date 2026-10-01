#include "Helpers.h"
#include "Interpreter.h"

#include <algorithm>
#include <array>
#include <cstdint>

// SIMD-group fragments, held whole: one dense row-major 8x8 per SIMD group
// rather than spread over its lanes as the D3D12/Vulkan fallback spreads it.
// A fill takes only a value and a fragment leaves only through a store, so the
// spreading is unobservable; the product is an 8x8 one per SIMD group, summed
// in the fallback's order: the accumulator first, then k = 0 to 7. A packed
// half or bf16 fragment is widened as it loads, so every fragment holds floats.

namespace eacp::GPU::CpuCompute
{
namespace
{
constexpr auto simdMatrixSide = static_cast<Word>(simdMatrixSize);

using SimdMatrixValues = std::array<float, Plan::fragmentElements>;

int simdMatrixFirstActiveLane(const Context& context,
                              const MaskFrame& frame,
                              int simdGroup)
{
    auto first = simdGroup * simdGroupWidth;
    auto end = std::min(first + simdGroupWidth, context.plan.lanes());

    for (auto lane = first; lane < end; ++lane)
        if (frame.mask[lane] != 0)
            return lane;

    return -1;
}

Word* simdMatrixFragment(const Context& context, int fragment, int simdGroup)
{
    return context.lanes(context.plan.fragment(fragment, simdGroup));
}

Word simdMatrixPatchElement(Word offset, Word stride, Word row, Word column)
{
    return offset + row * stride + column;
}

struct SimdMatrixPatch
{
    Word offset = 0;
    Word stride = 0;
};

SimdMatrixPatch
    simdMatrixPatchOf(const Context& context, const Plan::Step& step, int lane)
{
    return {context.lanes(context.plan.node(step.index))[lane],
            context.lanes(context.plan.node(step.stride))[lane]};
}

// The one place a fragment's element is read out of memory, a word at a time
// through readWord, which reads 0 past the end. A packed half or bf16 patch
// counts in sixteen-bit elements, as the fallback's eacpReadHalf and
// eacpReadBFloat16 walk do: the element is the word at element / 2, the half
// its parity picks, widened through the same helper.
template <typename ReadWord>
Word simdMatrixLoadedElement(SimdMatrixElement kind, Word element, ReadWord readWord)
{
    if (kind == SimdMatrixElement::Float)
        return readWord(element);

    auto word = readWord(element / 2u);
    auto parity = element % 2u;

    if (kind == SimdMatrixElement::Half)
        return Lanes::toWord(readHalf(word, parity));

    return Lanes::toWord(readBFloat16(word, parity));
}

template <typename Visit>
void simdMatrixForEachActiveGroup(const Context& context,
                                  const MaskFrame& frame,
                                  Visit visit)
{
    for (auto simdGroup = 0; simdGroup < context.plan.simdGroupCount(); ++simdGroup)
    {
        auto lane = simdMatrixFirstActiveLane(context, frame, simdGroup);

        if (lane >= 0)
            visit(simdGroup, lane);
    }
}

template <typename Visit>
void simdMatrixForEachElement(SimdMatrixPatch patch, Visit visit)
{
    for (auto row = Word {0}; row < simdMatrixSide; ++row)
        for (auto column = Word {0}; column < simdMatrixSide; ++column)
            visit(row * simdMatrixSide + column,
                  simdMatrixPatchElement(patch.offset, patch.stride, row, column));
}

void simdMatrixMultiplyAdd(Word* accumulator, const Word* left, const Word* right)
{
    auto product = SimdMatrixValues {};

    for (auto row = 0; row < simdMatrixSize; ++row)
    {
        for (auto column = 0; column < simdMatrixSize; ++column)
        {
            auto at = row * simdMatrixSize + column;
            auto sum = Lanes::toFloat(accumulator[at]);

            for (auto k = 0; k < simdMatrixSize; ++k)
            {
                auto term = Lanes::toFloat(left[row * simdMatrixSize + k]);
                sum += term * Lanes::toFloat(right[k * simdMatrixSize + column]);
            }

            product[static_cast<std::size_t>(at)] = sum;
        }
    }

    for (auto at = 0; at < Plan::fragmentElements; ++at)
        accumulator[at] = Lanes::toWord(product[static_cast<std::size_t>(at)]);
}
} // namespace

void fillSimdMatrix(const Context& context,
                    const Plan::Step& step,
                    const MaskFrame& frame)
{
    evaluateRange(context, step.scheduleBegin, step.scheduleEnd);

    const auto* values = context.lanes(context.plan.node(step.value));

    simdMatrixForEachActiveGroup(
        context,
        frame,
        [&](int simdGroup, int lane)
        {
            Lanes::fill(simdMatrixFragment(context, step.slot, simdGroup),
                        values[lane],
                        Plan::fragmentElements);
        });
}

void loadSimdMatrix(const Context& context,
                    const Plan::Step& step,
                    const MaskFrame& frame)
{
    evaluateRange(context, step.scheduleBegin, step.scheduleEnd);

    simdMatrixForEachActiveGroup(
        context,
        frame,
        [&](int simdGroup, int lane)
        {
            auto* fragment = simdMatrixFragment(context, step.slot, simdGroup);
            auto patch = simdMatrixPatchOf(context, step, lane);

            if (step.memory == SimdMatrixMemory::Shared)
            {
                const auto& shared = context.plan.sharedArrays()[step.buffer];
                const auto* storage = context.lanes(shared.storage);
                auto elements = static_cast<Word>(shared.elements);

                auto readWord = [&](Word word)
                { return word < elements ? storage[word] : 0u; };

                simdMatrixForEachElement(patch,
                                         [&](int at, Word element)
                                         {
                                             fragment[at] = simdMatrixLoadedElement(
                                                 step.element, element, readWord);
                                         });
                return;
            }

            const auto& view = context.slots[static_cast<std::size_t>(step.buffer)];

            auto readWord = [&](Word word)
            { return word < view.count ? view.load(word) : 0u; };

            simdMatrixForEachElement(patch,
                                     [&](int at, Word element)
                                     {
                                         fragment[at] = simdMatrixLoadedElement(
                                             step.element, element, readWord);
                                     });
        });
}

void storeSimdMatrix(const Context& context,
                     const Plan::Step& step,
                     const MaskFrame& frame)
{
    evaluateRange(context, step.scheduleBegin, step.scheduleEnd);

    simdMatrixForEachActiveGroup(
        context,
        frame,
        [&](int simdGroup, int lane)
        {
            const auto* fragment = simdMatrixFragment(context, step.slot, simdGroup);
            auto patch = simdMatrixPatchOf(context, step, lane);

            if (step.memory == SimdMatrixMemory::Shared)
            {
                const auto& shared = context.plan.sharedArrays()[step.buffer];
                auto* storage = context.lanes(shared.storage);
                auto elements = static_cast<Word>(shared.elements);

                simdMatrixForEachElement(patch,
                                         [&](int at, Word element)
                                         {
                                             if (element < elements)
                                                 storage[element] = fragment[at];
                                         });
                return;
            }

            const auto& view = context.slots[static_cast<std::size_t>(step.buffer)];

            simdMatrixForEachElement(patch,
                                     [&](int at, Word element)
                                     {
                                         if (element < view.count)
                                             view.store(element, fragment[at]);
                                     });
        });
}

void multiplyAddSimdMatrix(const Context& context,
                           const Plan::Step& step,
                           const MaskFrame& frame)
{
    simdMatrixForEachActiveGroup(
        context,
        frame,
        [&](int simdGroup, int)
        {
            simdMatrixMultiplyAdd(
                simdMatrixFragment(context, step.slot, simdGroup),
                simdMatrixFragment(context, step.left, simdGroup),
                simdMatrixFragment(context, step.right, simdGroup));
        });
}
} // namespace eacp::GPU::CpuCompute
