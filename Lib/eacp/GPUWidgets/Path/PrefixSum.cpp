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

// Grown when a batch needs more than the last one did, and never uploaded: the
// scan writes every element it reads, so what these hold on the way in is
// nobody's business.
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

    // How deep this one goes, decided before anything is dispatched: each level
    // is a thousandth of the one below it, so two of them reach a million
    // elements and three reach a billion.
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

    // What each level sums, and where it puts the result. The bottom one is the
    // caller's pair; every one above sums the totals the level below left.
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

        // Whole groups, because the kernel waits at a barrier and therefore has
        // no generated bounds guard of its own.
        pass.dispatch(block, levels[level].groups * lanes);
        ++dispatches;
    }

    auto& add = sharedKernel<ScanAddKernel>();

    // Down again, each level taking what the one above it summed. The top level
    // is a single group and is already the whole answer, so it starts below it.
    for (auto level = levelCount - 2; level >= 0; --level)
    {
        add.offsets = destinationOf(level);
        add.groupOffsets = destinationOf(level + 1);

        pass.dispatch(add, levels[level].count);
        ++dispatches;
    }
}
} // namespace eacp::GPUWidgets
