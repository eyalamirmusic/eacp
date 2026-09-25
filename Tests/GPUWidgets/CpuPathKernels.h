#pragma once

#include <eacp/GPU/CpuCompute/CpuCompute.h>
#include <eacp/GPUWidgets/GPUWidgets.h>

#include <NanoTest/NanoTest.h>

#include <algorithm>
#include <cstdint>
#include <source_location>
#include <span>
#include <string>

// The path kernels run on the CPU executor, and the element-for-element
// comparison of what they left against what the GPU left in the same buffers.
//
// Every kernel here is the library's own: nothing is re-derived, so a
// disagreement is either the executor or the GPU, never a transcription.
namespace eacp::GPUWidgets::cpu
{
using UInts = Vector<std::uint32_t>;

inline UInts uintsOf(int count, std::uint32_t value)
{
    auto values = UInts {};
    values.assign(count, value);
    return values;
}

// A kernel's executor, checked valid with the plan's own reason: a construct
// the executor refuses fails the case with that sentence rather than a count.
template <typename Kernel>
bool dispatch(Kernel& kernel,
              const GPU::CpuCompute::Bindings& bindings,
              int count,
              const std::string& what)
{
    auto executor = GPU::CpuCompute::Executor {kernel};
    nano::check(executor.isValid(), what + ": " + executor.reason());

    if (!executor.isValid())
        return false;

    auto ran = executor.dispatch(bindings, count);
    nano::check(ran, what + ": the cpu dispatch ran");
    return ran;
}

inline int scanGroupsFor(int count)
{
    return (count + ScanBlockKernel::perGroup - 1) / ScanBlockKernel::perGroup;
}

// PrefixSum::run, level for level, on the CPU: the same block dispatches up and
// the same add dispatches down, over the same kernels.
inline bool prefixSum(std::span<std::uint32_t> counts,
                      std::span<std::uint32_t> offsets,
                      int count)
{
    struct Level
    {
        int count = 0;
        int groups = 0;
        UInts totals;
        UInts offsets;
    };

    auto levels = Vector<Level> {};

    for (auto size = count; size > 0;)
    {
        auto level = Level {};
        level.count = size;
        level.groups = scanGroupsFor(size);
        level.totals = uintsOf(std::max(1, level.groups), 0u);

        if (!levels.empty())
            level.offsets = uintsOf(size, 0u);

        levels.add(std::move(level));

        if (levels.back().groups <= 1)
            break;

        size = levels.back().groups;
    }

    auto sourceOf = [&](int level) -> std::span<std::uint32_t>
    { return level == 0 ? counts : std::span {levels[level - 1].totals}; };

    auto destinationOf = [&](int level) -> std::span<std::uint32_t>
    { return level == 0 ? offsets : std::span {levels[level].offsets}; };

    auto block = ScanBlockKernel {};

    for (auto level = 0; level < levels.size(); ++level)
    {
        auto bindings = GPU::CpuCompute::Bindings {};
        bindings.set(block.counts, sourceOf(level));
        bindings.set(block.offsets, destinationOf(level));
        bindings.set(block.groupTotals, levels[level].totals);
        block.elementCount = (std::uint32_t) levels[level].count;

        if (!dispatch(block,
                      bindings,
                      levels[level].groups * ScanBlockKernel::lanes,
                      "ScanBlockKernel"))
            return false;
    }

    auto add = ScanAddKernel {};

    for (auto level = levels.size() - 2; level >= 0; --level)
    {
        auto bindings = GPU::CpuCompute::Bindings {};
        bindings.set(add.offsets, destinationOf(level));
        bindings.set(add.groupOffsets, destinationOf(level + 1));

        if (!dispatch(add, bindings, levels[level].count, "ScanAddKernel"))
            return false;
    }

    return true;
}

// Integer buffers agree exactly or not at all. What a failure says is the
// first index they part at and both values there, which is what tells an
// executor bug from an ordering the GPU never promised.
inline void
    expectSame(const UInts& cpu,
               const UInts& gpu,
               const std::string& what,
               const std::source_location& where = std::source_location::current())
{
    nano::check(cpu.size() == gpu.size(),
                what + ": " + std::to_string(cpu.size()) + " cpu elements against "
                    + std::to_string(gpu.size()) + " gpu",
                where);

    auto count = std::min(cpu.size(), gpu.size());
    auto differing = 0;
    auto first = -1;

    for (auto i = 0; i < count; ++i)
    {
        if (cpu[i] != gpu[i])
        {
            ++differing;

            if (first < 0)
                first = i;
        }
    }

    auto message = what + ": " + std::to_string(differing) + " of "
                   + std::to_string(count) + " differ";

    if (first >= 0)
        message += ", first at " + std::to_string(first) + " (cpu "
                   + std::to_string(cpu[first]) + ", gpu "
                   + std::to_string(gpu[first]) + ")";

    nano::check(differing == 0, message, where);
}

inline GPU::Buffer bufferOf(const UInts& values)
{
    return {GPU::Device::shared(),
            values.data(),
            (std::int64_t) sizeof(std::uint32_t) * values.size(),
            GPU::BufferUsage::Storage};
}

inline GPU::Buffer bufferOf(const Vector<float>& values)
{
    return {GPU::Device::shared(),
            values.data(),
            (std::int64_t) sizeof(float) * values.size(),
            GPU::BufferUsage::Storage};
}

template <typename T>
Vector<T> readBack(const GPU::Buffer& buffer, int count)
{
    auto values = Vector<T> {};
    values.resize(count);
    buffer.read(values.data(), (std::int64_t) sizeof(T) * count);
    return values;
}
} // namespace eacp::GPUWidgets::cpu
