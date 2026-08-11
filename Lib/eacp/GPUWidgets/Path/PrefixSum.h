#pragma once

#include "../Common.h"

namespace eacp::GPUWidgets
{
// One group's exclusive prefix sum: each thread sums its own perLane elements,
// the subtotals are scanned in threadgroup memory, then each thread rewrites its
// run. Leaves counts zeroed, which is what the counting sort needs as cursors.
struct ScanBlockKernel final : GPU::ComputeProgram
{
    // A thousand-fold reduction per level keeps the recursion two levels deep.
    static constexpr int lanes = GPU::ComputeProgram::groupWidth;
    static constexpr int perLane = 16;
    static constexpr int perGroup = lanes * perLane;

    ScanBlockKernel() { compile(); }

    void define() override
    {
        auto lane = localId();
        auto base = var(groupId() * (unsigned) perGroup + lane * (unsigned) perLane);

        auto mine = var(0u);

        for (auto i = 0; i < perLane; ++i)
        {
            auto at = base.get() + (unsigned) i;
            ifThen(at < elementCount, [&] { mine += counts.load(at); });
        }

        auto subtotals = shared<GPU::UInt>(lanes);
        write(subtotals, lane, mine.get());
        barrier();

        // Unrolled as the graph is built, so the barriers are statements of the
        // kernel: a barrier in a loop threads may leave early is undefined.
        for (auto step = 1; step < lanes; step <<= 1)
        {
            auto carried = var(0u);

            ifThen(lane >= (unsigned) step,
                   [&]
                   {
                       // Clamped so the index cannot underflow even unguarded.
                       carried =
                           subtotals[max(lane, (unsigned) step) - (unsigned) step];
                   });

            barrier();
            write(subtotals, lane, subtotals[lane] + carried.get());
            barrier();
        }

        // Inclusive minus its own is where this thread's run starts.
        auto running = var(subtotals[lane] - mine.get());

        for (auto i = 0; i < perLane; ++i)
        {
            auto at = base.get() + (unsigned) i;

            ifThen(at < elementCount,
                   [&]
                   {
                       auto value = counts.load(at);
                       write(offsets, at, running.get());
                       running += value;
                       write(counts, at, 0u);
                   });
        }

        // The group's total, for the level above; the last lane holds it.
        ifThen(
            lane == (unsigned) (lanes - 1),
            [&]
            { write(groupTotals, groupId(), subtotals[(unsigned) (lanes - 1)]); });
    }

    // counts is read, summed and left zeroed; offsets takes the exclusive prefix
    // within each group, groupTotals one number per group.
    GPU::Uniform<GPU::AtomicBuffer> counts;
    GPU::Uniform<GPU::AtomicBuffer> offsets;
    GPU::Uniform<GPU::AtomicBuffer> groupTotals;

    // A kernel with a barrier gets no generated bounds guard - every thread must
    // reach every barrier - so the dispatch rounds up and this holds the stores.
    GPU::Uniform<GPU::UInt> elementCount;

    EACP_SHADER(counts, offsets, groupTotals, elementCount)
};

// Adds the level above back down: every element takes its group's running total.
struct ScanAddKernel final : GPU::ComputeProgram
{
    ScanAddKernel() { compile(); }

    void define() override
    {
        auto at = threadId();
        auto group = at / (unsigned) ScanBlockKernel::perGroup;

        write(offsets, at, offsets.load(at) + groupOffsets.load(group));
    }

    GPU::Uniform<GPU::AtomicBuffer> offsets;
    GPU::Uniform<GPU::AtomicBuffer> groupOffsets;

    EACP_SHADER(offsets, groupOffsets)
};

// The levels of the scan, and the buffers between them, kept between frames.
class PrefixSum
{
public:
    PrefixSum() = default;

    // Records the exclusive sum of counts[0, count) into offsets and leaves
    // counts zeroed. Both are the caller's and must hold count unsigned ints.
    void run(GPU::ComputePass& pass,
             const GPU::Buffer& counts,
             const GPU::Buffer& offsets,
             int count);

    int getDispatchCount() const { return dispatches; }

private:
    struct Level
    {
        int count = 0;
        int groups = 0;
        std::optional<GPU::Buffer> totals;
        std::optional<GPU::Buffer> offsets;
    };

    // A thousandth of the rung below each time, so four reach 10^12 elements.
    static constexpr int maxLevels = 4;

    std::array<Level, maxLevels> levels;
    int levelCount = 0;
    int dispatches = 0;
};
} // namespace eacp::GPUWidgets
