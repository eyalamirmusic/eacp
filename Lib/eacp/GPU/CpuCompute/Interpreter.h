#pragma once

#include "Helpers.h"
#include "Lanes.h"
#include "Plan.h"

#include <array>
#include <atomic>
#include <cstddef>
#include <cstring>

// Internal: the per-dispatch context shared by the executor's translation units.

namespace eacp::GPU::CpuCompute
{
static_assert(std::atomic_ref<Word>::is_always_lock_free);
static_assert(std::atomic_ref<Word>::required_alignment == alignof(Word));

// What an atomic operation leaves in the element, given what it held: Max and
// Min compare as signed on an Int element, and the rest are the same bits
// whichever the element is.
constexpr Word
    atomicCombine(AtomicOperation operation, bool isSigned, Word old, Word value)
{
    auto oldSigned = Lanes::toSigned(old);
    auto valueSigned = Lanes::toSigned(value);

    switch (operation)
    {
        case AtomicOperation::Add:
            return old + value;
        case AtomicOperation::Sub:
            return old - value;
        case AtomicOperation::Max:
            if (isSigned)
                return oldSigned > valueSigned ? old : value;

            return old > value ? old : value;
        case AtomicOperation::Min:
            if (isSigned)
                return oldSigned < valueSigned ? old : value;

            return old < value ? old : value;
        case AtomicOperation::And:
            return old & value;
        case AtomicOperation::Or:
            return old | value;
        case AtomicOperation::Xor:
            return old ^ value;
        case AtomicOperation::Exchange:
        case AtomicOperation::CompareExchange:
            return value;
    }

    return old;
}

struct AtomicOutcome
{
    Word previous = 0;
    bool exchanged = false;
};

// A bound storage slot. An f16 one holds two-byte elements, loaded widened to
// the float lane they are computed in and stored narrowed back - exactly, the
// lane having been rounded to a half where it was computed.
struct SlotView
{
    Word load(Word element) const
    {
        if (half)
            return Lanes::toWord(widenHalf(loadHalfBits(element)));

        auto word = Word {};
        std::memcpy(&word, data + sizeof(Word) * element, sizeof(Word));
        return word;
    }

    void store(Word element, Word word) const
    {
        if (half)
        {
            auto bits = narrowToHalf(Lanes::toFloat(word));
            std::memcpy(data + sizeof(bits) * element, &bits, sizeof(bits));
            return;
        }

        std::memcpy(data + sizeof(Word) * element, &word, sizeof(Word));
    }

    std::uint16_t loadHalfBits(Word element) const
    {
        auto bits = std::uint16_t {};
        std::memcpy(&bits, data + sizeof(bits) * element, sizeof(bits));
        return bits;
    }

    // Only an Atomic slot takes these, and it is bound from a span of uint32_t
    // or int32_t, so the words are real, aligned 32-bit integers - which may
    // alias as one another. Groups on other threads reach the same words, so
    // every access, the store included, is an atomic one.
    Word atomicLoad(Word element) const
    {
        return std::atomic_ref<Word>(atomicWord(element))
            .load(std::memory_order_relaxed);
    }

    void atomicStore(Word element, Word value) const
    {
        std::atomic_ref<Word>(atomicWord(element))
            .store(value, std::memory_order_relaxed);
    }

    AtomicOutcome atomicApply(Word element,
                              AtomicOperation operation,
                              bool isSigned,
                              Word value,
                              Word compare) const
    {
        constexpr auto relaxed = std::memory_order_relaxed;
        auto word = std::atomic_ref<Word>(atomicWord(element));

        switch (operation)
        {
            case AtomicOperation::Add:
                return {word.fetch_add(value, relaxed)};
            case AtomicOperation::Sub:
                return {word.fetch_sub(value, relaxed)};
            case AtomicOperation::And:
                return {word.fetch_and(value, relaxed)};
            case AtomicOperation::Or:
                return {word.fetch_or(value, relaxed)};
            case AtomicOperation::Xor:
                return {word.fetch_xor(value, relaxed)};
            case AtomicOperation::Exchange:
                return {word.exchange(value, relaxed)};
            case AtomicOperation::CompareExchange:
            {
                auto expected = compare;
                auto exchanged =
                    word.compare_exchange_strong(expected, value, relaxed, relaxed);
                return {expected, exchanged};
            }
            case AtomicOperation::Max:
            case AtomicOperation::Min:
                break;
        }

        auto old = word.load(relaxed);

        while (!word.compare_exchange_weak(
            old, atomicCombine(operation, isSigned, old, value), relaxed, relaxed))
        {
        }

        return {old};
    }

    Word& atomicWord(Word element) const
    {
        return reinterpret_cast<Word*>(data)[element];
    }

    // A struct slot's flat component `which` of `element`, at the byte its
    // layout gives it: an f16 widened and narrowed as a half slot's is.
    Word loadStruct(Word element, int which) const
    {
        const auto& component = components[which];
        const auto* at = data + std::size_t {element} * structStride
                         + static_cast<std::size_t>(component.offset);

        if (component.half)
        {
            auto bits = std::uint16_t {};
            std::memcpy(&bits, at, sizeof(bits));
            return Lanes::toWord(widenHalf(bits));
        }

        auto word = Word {};
        std::memcpy(&word, at, sizeof(Word));
        return word;
    }

    void storeStruct(Word element, int which, Word word) const
    {
        const auto& component = components[which];
        auto* at = data + std::size_t {element} * structStride
                   + static_cast<std::size_t>(component.offset);

        if (component.half)
        {
            auto bits = narrowToHalf(Lanes::toFloat(word));
            std::memcpy(at, &bits, sizeof(bits));
            return;
        }

        std::memcpy(at, &word, sizeof(Word));
    }

    std::byte* data = nullptr;
    std::uint32_t count = 0;
    bool half = false;
    std::size_t structStride = 0;
    const StructComponent* components = nullptr;
};

struct Context
{
    const Plan& plan;
    Word* words = nullptr;
    int stride = 0;
    std::array<SlotView, Plan::maxSlots> slots {};

    constexpr Word* lanes(std::uint32_t offset) const { return words + offset; }

    constexpr Word* lanes(const Plan::Node& node, int component = 0) const
    {
        return words + node.scratch
               + static_cast<std::size_t>(component)
                     * static_cast<std::size_t>(stride);
    }

    const Plan::Node& argumentNode(const Plan::Node& node, int which) const
    {
        return plan.node(plan.argument(node, which));
    }

    const Word* operand(const Plan::Node& node, int which, int component) const
    {
        const auto& argument = argumentNode(node, which);
        return lanes(argument, argument.components == 1 ? 0 : component);
    }
};

// The lanes of a ramp whose element is in range, with the element of the first
// one; within a run the element grows by the ramp's scale and never wraps.
struct LaneRun
{
    int begin = 0;
    int end = 0;
    Word element = 0;
};

struct LaneRuns
{
    std::array<LaneRun, 2> runs {};
    int count = 0;
};

// Element start + lane * scale, taken wide so the one wrap a ramp spanning less
// than 2^32 can make is a second run rather than a jump inside one.
class RampBounds
{
public:
    constexpr RampBounds(Word startToUse, Word scaleToUse, int lanesToUse)
        : start(startToUse)
        , scale(scaleToUse)
        , lanes(lanesToUse)
    {
    }

    constexpr LaneRuns inRange(Word size) const
    {
        constexpr auto wrap = std::uint64_t {1} << 32;
        auto result = LaneRuns {};
        addRun(result, 0, size);
        addRun(result, wrap, wrap + size);
        return result;
    }

private:
    constexpr int firstLaneReaching(std::uint64_t element) const
    {
        if (start >= element)
            return 0;

        if (scale == 0)
            return lanes;

        auto lane = (element - start + scale - 1) / scale;
        return lane < static_cast<std::uint64_t>(lanes) ? static_cast<int>(lane)
                                                        : lanes;
    }

    constexpr void
        addRun(LaneRuns& result, std::uint64_t low, std::uint64_t high) const
    {
        auto begin = firstLaneReaching(low);
        auto end = firstLaneReaching(high);

        if (begin >= end)
            return;

        auto element = start + static_cast<std::uint64_t>(begin) * scale;
        result.runs[static_cast<std::size_t>(result.count++)] = {
            begin, end, static_cast<Word>(element)};
    }

    std::uint64_t start = 0;
    std::uint64_t scale = 0;
    int lanes = 0;
};

// Whether an index row the plan could not prove a ramp is one anyway, as
// (i + 1) % length is on every group but the one that wraps.
constexpr bool isContiguousRow(const Word* indices, int lanes)
{
    auto first = indices[0];
    auto last = lanes - 1;

    if (indices[last] != first + static_cast<Word>(last))
        return false;

    auto differs = Word {0};

    for (auto lane = 0; lane < lanes; ++lane)
        differs |= indices[lane] ^ (first + static_cast<Word>(lane));

    return differs == 0;
}

struct MaskFrame
{
    Word* mask = nullptr;
    MaskFrame* parent = nullptr;
};

struct LoopFrame
{
    MaskFrame* iteration = nullptr;
    Word* live = nullptr;
    const LoopFrame* outer = nullptr;
};

void evaluateRange(const Context& context, int begin, int end);
void evaluateCall(const Context& context, const Plan::Node& node);

// A SIMD group acts whole, taking its first active lane's operands, when any
// of its lanes is active under the mask, and not at all when none is.
void fillSimdMatrix(const Context& context,
                    const Plan::Step& step,
                    const MaskFrame& frame);
void loadSimdMatrix(const Context& context,
                    const Plan::Step& step,
                    const MaskFrame& frame);
void storeSimdMatrix(const Context& context,
                     const Plan::Step& step,
                     const MaskFrame& frame);
void multiplyAddSimdMatrix(const Context& context,
                           const Plan::Step& step,
                           const MaskFrame& frame);

void runBlock(const Context& context,
              int block,
              MaskFrame& frame,
              int depth,
              const LoopFrame* loop);
} // namespace eacp::GPU::CpuCompute
