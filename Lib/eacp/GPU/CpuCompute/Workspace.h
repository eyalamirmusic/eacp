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
    constexpr std::uint64_t planSerial() const { return serial; }

    constexpr Word* words() { return base; }
    constexpr const Word* words() const { return base; }

    constexpr Word* at(std::uint32_t offset) { return base + offset; }
    constexpr const Word* at(std::uint32_t offset) const { return base + offset; }

    std::size_t sizeInBytes() const;

private:
    Vector<Word> storage;
    Word* base = nullptr;
    std::uint64_t serial = 0;
};
} // namespace eacp::GPU::CpuCompute
