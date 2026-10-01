#pragma once

#include "Workspace.h"

#include <array>
#include <atomic>
#include <cstddef>
#include <cstring>

// Internal: the per-dispatch context shared by the executor's translation units.

namespace eacp::GPU::CpuCompute
{
static_assert(std::atomic_ref<Word>::is_always_lock_free);
static_assert(std::atomic_ref<Word>::required_alignment == alignof(Word));

struct SlotView
{
    Word load(Word element) const
    {
        auto word = Word {};
        std::memcpy(&word, data + sizeof(Word) * element, sizeof(Word));
        return word;
    }

    void store(Word element, Word word) const
    {
        std::memcpy(data + sizeof(Word) * element, &word, sizeof(Word));
    }

    // Only an Atomic slot takes these, and it is bound from a span of
    // uint32_t, so the words are real, aligned uint32_t objects.
    Word atomicLoad(Word element) const
    {
        return std::atomic_ref<Word>(atomicWord(element))
            .load(std::memory_order_relaxed);
    }

    Word atomicAdd(Word element, Word value) const
    {
        return std::atomic_ref<Word>(atomicWord(element))
            .fetch_add(value, std::memory_order_relaxed);
    }

    Word& atomicWord(Word element) const
    {
        return reinterpret_cast<Word*>(data)[element];
    }

    std::byte* data = nullptr;
    std::uint32_t count = 0;
};

struct Context
{
    const Plan& plan;
    Word* words = nullptr;
    int stride = 0;
    std::array<SlotView, Plan::maxSlots> slots {};

    Word* lanes(std::uint32_t offset) const { return words + offset; }

    Word* lanes(const Plan::Node& node, int component = 0) const
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
    RampBounds(Word startToUse, Word scaleToUse, int lanesToUse)
        : start(startToUse)
        , scale(scaleToUse)
        , lanes(lanesToUse)
    {
    }

    LaneRuns inRange(Word size) const
    {
        constexpr auto wrap = std::uint64_t {1} << 32;
        auto result = LaneRuns {};
        addRun(result, 0, size);
        addRun(result, wrap, wrap + size);
        return result;
    }

private:
    int firstLaneReaching(std::uint64_t element) const
    {
        if (start >= element)
            return 0;

        if (scale == 0)
            return lanes;

        auto lane = (element - start + scale - 1) / scale;
        return lane < static_cast<std::uint64_t>(lanes) ? static_cast<int>(lane)
                                                        : lanes;
    }

    void addRun(LaneRuns& result, std::uint64_t low, std::uint64_t high) const
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
inline bool isContiguousRow(const Word* indices, int lanes)
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
