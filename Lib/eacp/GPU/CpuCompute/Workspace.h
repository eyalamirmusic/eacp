#pragma once

#include "Plan.h"

// Every word a run writes: the lanes of each node, the variables, the arrays,
// the shared arrays and the reduction scratch, the mask frames, the local
// coordinates and the uniform words. Allocated once,
// to the plan's layout, and never resized; a plan can have any number of these.

namespace eacp::GPU::CpuCompute
{
class Workspace
{
public:
    explicit Workspace(const Plan& plan);

    Word* words() { return base; }
    const Word* words() const { return base; }

    Word* at(std::uint32_t offset) { return base + offset; }
    const Word* at(std::uint32_t offset) const { return base + offset; }

    Word* uniformWords(const Plan& plan, int slot)
    {
        return base + plan.uniformWords()
               + static_cast<std::size_t>(slot * Plan::uniformWordsPerSlot);
    }

    std::size_t sizeInBytes() const { return storage.size() * sizeof(Word); }

private:
    Vector<Word> storage;
    Word* base = nullptr;
};
} // namespace eacp::GPU::CpuCompute
