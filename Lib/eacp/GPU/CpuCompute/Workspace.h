#pragma once

#include "Plan.h"

// Every word a run writes: the lanes of each node, the variables, the arrays,
// the shared arrays, the SIMD-group fragments and the reduction scratch, the
// mask frames and the local coordinates. Allocated once, to the plan's layout,
// and never resized; a plan can have any number of these, one per thread that
// runs Executor::dispatchGroups.

namespace eacp::GPU::CpuCompute
{
class Workspace
{
public:
    explicit Workspace(const Plan& plan);

    Workspace(const Workspace&) = delete;
    Workspace& operator=(const Workspace&) = delete;
    // A moved-from workspace matches no plan.
    Workspace(Workspace&& other) noexcept;
    Workspace& operator=(Workspace&& other) noexcept;

    // The serial of the plan this was laid out for.
    std::uint64_t planSerial() const { return serial; }

    Word* words() { return base; }
    const Word* words() const { return base; }

    Word* at(std::uint32_t offset) { return base + offset; }
    const Word* at(std::uint32_t offset) const { return base + offset; }

    std::size_t sizeInBytes() const { return storage.size() * sizeof(Word); }

private:
    Vector<Word> storage;
    Word* base = nullptr;
    std::uint64_t serial = 0;
};
} // namespace eacp::GPU::CpuCompute
