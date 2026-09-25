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

void runBlock(const Context& context,
              int block,
              MaskFrame& frame,
              int depth,
              const LoopFrame* loop);
} // namespace eacp::GPU::CpuCompute
