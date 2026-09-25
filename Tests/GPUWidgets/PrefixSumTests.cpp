#include "CpuPathKernels.h"

#include <eacp/GPUWidgets/GPUWidgets.h>

#include <NanoTest/NanoTest.h>

#include <string>

// The prefix sum the binner's counts go through, on its own.
//
// It is covered by every rasterization test in this directory already - a tile
// whose offset is wrong reads somebody else's segments - but only at the sizes
// those paths happen to have, and only through a picture. What is checked here
// is the thing a picture cannot say: that the levels compose. A scan that
// summed each group of 1024 correctly and never added what the groups before it
// came to draws a perfectly plausible mask for every path whose tiles fit in one
// group, which is most of them.
//
// So the sizes below straddle the group: one element, one short of a group, a
// group exactly, one past it, and past the square of it - which is the first
// size that needs three levels rather than two.
//
// And the counts are not all ones. A sum that dropped its input and counted
// instead would pass on an array of ones at every size in the list.

using namespace nano;
using namespace eacp;
using namespace eacp::GPUWidgets;

namespace
{
constexpr auto perGroup = ScanBlockKernel::perGroup;

// Something with a shape to it, so a scan that lost an element is a different
// number rather than the same one.
Vector<std::uint32_t> countsOfLength(int count)
{
    auto values = Vector<std::uint32_t> {};

    for (auto i = 0; i < count; ++i)
        values.add((std::uint32_t) ((i * 7 + (i % 13)) % 31));

    return values;
}

struct Result
{
    Vector<std::uint32_t> offsets;
    Vector<std::uint32_t> source;
};

Result scanOnGpu(const Vector<std::uint32_t>& counts)
{
    auto result = Result {};
    auto bytes = (int) sizeof(std::uint32_t) * counts.size();

    auto source = GPU::Buffer {
        GPU::Device::shared(), counts.data(), bytes, GPU::BufferUsage::Storage};
    auto destination = GPU::Buffer {
        GPU::Device::shared(), nullptr, bytes, GPU::BufferUsage::Storage};

    auto sum = PrefixSum {};
    auto commands = GPU::Device::shared().makeCommandBuffer();

    {
        auto pass = commands.beginCompute();
        sum.run(pass, source, destination, counts.size());
    }

    commands.commit();

    result.offsets.resize(counts.size());
    result.source.resize(counts.size());

    // A read is what both backends wait for; commit() returns as soon as the
    // list is on the queue on D3D12.
    destination.read(result.offsets.data(), bytes);
    source.read(result.source.data(), bytes);
    return result;
}

void expectScans(int count)
{
    if (!GPU::Device::shared().isValid())
        return;

    auto counts = countsOfLength(count);
    auto result = scanOnGpu(counts);

    auto running = std::uint32_t {0};
    auto wrong = 0;
    auto firstWrong = -1;
    auto leftBehind = 0;

    for (auto i = 0; i < count; ++i)
    {
        if (result.offsets[i] != running)
        {
            ++wrong;

            if (firstWrong < 0)
                firstWrong = i;
        }

        if (result.source[i] != 0)
            ++leftBehind;

        running += counts[i];
    }

    auto where = std::to_string(wrong) + " of " + std::to_string(count)
                 + " wrong, first at " + std::to_string(firstWrong);

    check(wrong == 0, where);

    // The counting sort that follows hands its slots out through this very
    // array, so a scan that read its input without clearing it would have every
    // tile's cursor start at that tile's own count.
    check(leftBehind == 0, std::to_string(leftBehind) + " elements not left zeroed");
}
} // namespace

// One group's worth and less, which is the whole of the sum for any path an
// interface draws.
auto tWithinOneGroup = test("PrefixSum/withinOneGroup") = []
{
    for (auto count: {1, 2, 63, 64, 65, 255, perGroup - 1, perGroup})
        expectScans(count);
};

// Past it, which is where the level above has to carry what the groups below it
// came to. A window-sized path is a couple of thousand tiles and lands here.
auto tAcrossGroups = test("PrefixSum/acrossGroups") = []
{
    for (auto count: {perGroup + 1, perGroup * 2, perGroup * 7 + 3})
        expectScans(count);
};

// And past the square of the group, which is the first size that is three levels
// deep rather than two - a canvas of a hundred and twenty-eight full-width lanes
// is four hundred thousand tiles and lands here.
auto tThreeLevels = test("PrefixSum/pastTheSquareOfAGroup") = []
{ expectScans(perGroup * perGroup + 17); };

// ---------------------------------------------------------------------------
// The same kernels on the CPU executor, held to the GPU's buffers element for
// element. The CPU half always runs and is held to the running sum as well, so
// a lane with no device still checks it.

namespace
{
const auto everySize = std::initializer_list<int> {1,
                                                   2,
                                                   63,
                                                   64,
                                                   65,
                                                   255,
                                                   perGroup - 1,
                                                   perGroup,
                                                   perGroup + 1,
                                                   perGroup * 2,
                                                   perGroup * 7 + 3,
                                                   perGroup * perGroup + 17};

bool hasDevice()
{
    return GPU::Device::shared().isValid();
}

// One level of the block scan, on its own: the offsets within each group, the
// counts left zeroed and the total per group.
struct BlockResult
{
    cpu::UInts counts;
    cpu::UInts offsets;
    cpu::UInts totals;
};

constexpr auto untouched = 0xdeadbeefu;

BlockResult scanBlockOnCpu(const cpu::UInts& counts)
{
    auto count = counts.size();
    auto groups = cpu::scanGroupsFor(count);
    auto result = BlockResult {
        counts, cpu::uintsOf(count, untouched), cpu::uintsOf(groups, untouched)};

    auto kernel = ScanBlockKernel {};
    auto bindings = GPU::CpuCompute::Bindings {};
    bindings.set(kernel.counts, result.counts);
    bindings.set(kernel.offsets, result.offsets);
    bindings.set(kernel.groupTotals, result.totals);
    kernel.elementCount = (std::uint32_t) count;

    cpu::dispatch(kernel, bindings, groups * ScanBlockKernel::lanes, "ScanBlock");
    return result;
}

BlockResult scanBlockOnGpu(const cpu::UInts& counts)
{
    auto count = counts.size();
    auto groups = cpu::scanGroupsFor(count);

    auto source = cpu::bufferOf(counts);
    auto offsets = cpu::bufferOf(cpu::uintsOf(count, untouched));
    auto totals = cpu::bufferOf(cpu::uintsOf(groups, untouched));

    auto& kernel = sharedKernel<ScanBlockKernel>();
    kernel.counts = source;
    kernel.offsets = offsets;
    kernel.groupTotals = totals;
    kernel.elementCount = (std::uint32_t) count;

    auto commands = GPU::Device::shared().makeCommandBuffer();

    {
        auto pass = commands.beginCompute();
        pass.dispatch(kernel, groups * ScanBlockKernel::lanes);
    }

    commands.commit();

    return {cpu::readBack<std::uint32_t>(source, count),
            cpu::readBack<std::uint32_t>(offsets, count),
            cpu::readBack<std::uint32_t>(totals, groups)};
}

cpu::UInts scanAddOnCpu(const cpu::UInts& offsets, const cpu::UInts& groupOffsets)
{
    auto result = offsets;
    auto groups = groupOffsets;

    auto kernel = ScanAddKernel {};
    auto bindings = GPU::CpuCompute::Bindings {};
    bindings.set(kernel.offsets, result);
    bindings.set(kernel.groupOffsets, groups);

    cpu::dispatch(kernel, bindings, offsets.size(), "ScanAdd");
    return result;
}

cpu::UInts scanAddOnGpu(const cpu::UInts& offsets, const cpu::UInts& groupOffsets)
{
    auto destination = cpu::bufferOf(offsets);
    auto groups = cpu::bufferOf(groupOffsets);

    auto& kernel = sharedKernel<ScanAddKernel>();
    kernel.offsets = destination;
    kernel.groupOffsets = groups;

    auto commands = GPU::Device::shared().makeCommandBuffer();

    {
        auto pass = commands.beginCompute();
        pass.dispatch(kernel, offsets.size());
    }

    commands.commit();
    return cpu::readBack<std::uint32_t>(destination, offsets.size());
}
} // namespace

auto tScanBlockOnCpu = test("PrefixSum/scanBlockOnTheCpuMatchesTheGpu") = []
{
    for (auto count: everySize)
    {
        auto counts = countsOfLength(count);
        auto onCpu = scanBlockOnCpu(counts);
        auto size = " at " + std::to_string(count);

        check(onCpu.counts == cpu::uintsOf(count, 0u), "cpu counts zeroed" + size);

        if (!hasDevice())
            continue;

        auto onGpu = scanBlockOnGpu(counts);

        cpu::expectSame(onCpu.offsets, onGpu.offsets, "offsets" + size);
        cpu::expectSame(onCpu.counts, onGpu.counts, "counts" + size);
        cpu::expectSame(onCpu.totals, onGpu.totals, "group totals" + size);
    }
};

// Offsets and group offsets with a shape to them rather than a scan's output,
// so an add that took the wrong group's offset is a different number.
auto tScanAddOnCpu = test("PrefixSum/scanAddOnTheCpuMatchesTheGpu") = []
{
    for (auto count: everySize)
    {
        auto offsets = countsOfLength(count);
        auto groupOffsets = countsOfLength(cpu::scanGroupsFor(count) + 3);

        for (auto& value: groupOffsets)
            value *= 1000u;

        auto onCpu = scanAddOnCpu(offsets, groupOffsets);
        auto size = " at " + std::to_string(count);

        for (auto i = 0; i < count; ++i)
            offsets[i] += groupOffsets[i / perGroup];

        check(onCpu == offsets, "cpu adds its group's offset" + size);

        if (!hasDevice())
            continue;

        auto onGpu = scanAddOnGpu(countsOfLength(count), groupOffsets);
        cpu::expectSame(onCpu, onGpu, "offsets" + size);
    }
};

// The whole of PrefixSum - every level up and every level down - on the CPU,
// against the library's own GPU run over the same counts.
auto tWholeSumOnCpu = test("PrefixSum/theWholeSumOnTheCpuMatchesTheGpu") = []
{
    for (auto count: everySize)
    {
        auto counts = countsOfLength(count);
        auto source = counts;
        auto offsets = cpu::uintsOf(count, untouched);
        auto size = " at " + std::to_string(count);

        check(cpu::prefixSum(source, offsets, count), "cpu sum ran" + size);

        auto running = std::uint32_t {0};
        auto expected = cpu::uintsOf(count, 0u);

        for (auto i = 0; i < count; ++i)
        {
            expected[i] = running;
            running += counts[i];
        }

        check(offsets == expected, "cpu offsets are the running sum" + size);
        check(source == cpu::uintsOf(count, 0u), "cpu counts zeroed" + size);

        if (!hasDevice())
            continue;

        auto onGpu = scanOnGpu(counts);
        cpu::expectSame(offsets, onGpu.offsets, "offsets" + size);
        cpu::expectSame(source, onGpu.source, "counts" + size);
    }
};
