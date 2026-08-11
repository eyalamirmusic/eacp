#include "PrefixSum.h"

#include "CoverageKernel.h"

#include <cassert>

namespace eacp::GPUWidgets
{
namespace
{
constexpr auto perGroup = ScanBlockKernel::perGroup;
constexpr auto lanes = ScanBlockKernel::lanes;

int groupsFor(int count)
{
    return (count + perGroup - 1) / perGroup;
}

// Never uploaded: the scan writes every element it reads.
void ensureRoom(std::optional<GPU::Buffer>& buffer, int count)
{
    auto bytes = sizeof(std::uint32_t) * (std::size_t) std::max(1, count);

    if (!buffer.has_value() || buffer->size() < bytes)
        buffer.emplace(
            GPU::Device::shared(), nullptr, bytes, GPU::BufferUsage::Storage);
}
} // namespace

void PrefixSum::run(GPU::ComputePass& pass,
                    const GPU::Buffer& counts,
                    const GPU::Buffer& offsets,
                    int count)
{
    dispatches = 0;
    levelCount = 0;

    if (count <= 0)
        return;

    // The depth is settled before anything is dispatched.
    for (auto size = count; levelCount < maxLevels;
         size = levels[levelCount - 1].groups)
    {
        auto& level = levels[levelCount];
        level.count = size;
        level.groups = groupsFor(size);
        ensureRoom(level.totals, level.groups);

        if (levelCount > 0)
            ensureRoom(level.offsets, size);

        ++levelCount;

        if (level.groups <= 1)
            break;
    }

    assert(levels[levelCount - 1].groups <= 1
           && "eacp: a prefix sum too large for its levels");

    // Level zero is the caller's pair; each one above sums the totals below it.
    auto sourceOf = [&](int level) -> const GPU::Buffer&
    { return level == 0 ? counts : *levels[level - 1].totals; };

    auto destinationOf = [&](int level) -> const GPU::Buffer&
    { return level == 0 ? offsets : *levels[level].offsets; };

    auto& block = sharedKernel<ScanBlockKernel>();

    for (auto level = 0; level < levelCount; ++level)
    {
        block.counts = sourceOf(level);
        block.offsets = destinationOf(level);
        block.groupTotals = *levels[level].totals;
        block.elementCount = (std::uint32_t) levels[level].count;

        // Whole groups: a kernel with a barrier gets no generated bounds guard.
        pass.dispatch(block, levels[level].groups * lanes);
        ++dispatches;
    }

    auto& add = sharedKernel<ScanAddKernel>();

    // The top level is one group and already complete, so the walk down skips it.
    for (auto level = levelCount - 2; level >= 0; --level)
    {
        add.offsets = destinationOf(level);
        add.groupOffsets = destinationOf(level + 1);

        pass.dispatch(add, levels[level].count);
        ++dispatches;
    }
}
} // namespace eacp::GPUWidgets
