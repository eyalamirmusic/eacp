#pragma once

#include "../Common.h"

namespace eacp::GPUWidgets
{
// An exclusive prefix sum over a buffer of unsigned integers, on the GPU.
//
// It is what turns a count per tile into an offset per tile: after it, a tile's
// segments live at [offsets[t], offsets[t + 1]) of one array, which is the
// layout a coverage thread walks. The counts are what a binning kernel produced
// with atomic adds, so nothing about them ever reached the CPU and neither does
// this - the sum is taken between two dispatches of the same pass.
//
// A group of 64 threads owns 1024 consecutive elements: each thread sums its own
// 16, the 64 subtotals are scanned in threadgroup memory, and each thread walks
// its 16 again writing the running total. What is left is one number per group,
// which is the same problem one thousandth the size - so the levels recurse until
// a single group covers the whole of it. Two levels reach a million elements,
// which is past what a batch of paths can allocate at all.
//
// **The counts are left zeroed.** A scan reads every element it sums, so writing
// a zero behind it costs nothing and gives the stage after this one its cursors:
// a counting sort needs a counter per tile starting at zero, and that is exactly
// the array whose counts were just consumed.
struct ScanBlockKernel final : GPU::ComputeProgram
{
    // 64 threads is the group the 1D dispatch uses, and sixteen elements each is
    // what makes the levels shallow: a thousand-fold reduction per level means a
    // batch of any size a texture can hold is two of them.
    static constexpr int lanes = GPU::ComputeProgram::groupWidth;
    static constexpr int perLane = 16;
    static constexpr int perGroup = lanes * perLane;

    ScanBlockKernel() { compile(); }

    void define() override
    {
        auto lane = localId();
        auto base = var(groupId() * (unsigned) perGroup + lane * (unsigned) perLane);

        // What this thread's own sixteen come to, which is the only thing the
        // group has to agree about before it can scan.
        auto mine = var(0u);

        for (auto i = 0; i < perLane; ++i)
        {
            auto at = base.get() + (unsigned) i;
            ifThen(at < elementCount, [&] { mine += counts.load(at); });
        }

        auto subtotals = shared<GPU::UInt>(lanes);
        write(subtotals, lane, mine.get());
        barrier();

        // Six doublings turn the subtotals into their own inclusive prefix. The
        // C++ loop is unrolled as the graph is built, so the barriers below are
        // statements of the kernel rather than iterations of one - which they
        // have to be, a barrier inside a loop some threads leave early being
        // undefined on both backends.
        for (auto step = 1; step < lanes; step <<= 1)
        {
            auto carried = var(0u);

            ifThen(lane >= (unsigned) step,
                   [&]
                   {
                       // Clamped rather than trusted: the subscript is inside the
                       // guard, but an index that underflows when the guard is
                       // false is a shape worth not writing down at all.
                       carried =
                           subtotals[max(lane, (unsigned) step) - (unsigned) step];
                   });

            barrier();
            write(subtotals, lane, subtotals[lane] + carried.get());
            barrier();
        }

        // Inclusive minus its own is exclusive, which is where this thread's run
        // of sixteen starts.
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

        // The group's total, for the level above to scan. Written by the lane
        // holding the inclusive sum of all of them, which is the last.
        ifThen(
            lane == (unsigned) (lanes - 1),
            [&]
            { write(groupTotals, groupId(), subtotals[(unsigned) (lanes - 1)]); });
    }

    // Read, summed and left zeroed. offsets takes the exclusive prefix within
    // each group; groupTotals takes one number per group, which is what makes
    // the level above's sum the same problem.
    GPU::Uniform<GPU::AtomicBuffer> counts;
    GPU::Uniform<GPU::AtomicBuffer> offsets;
    GPU::Uniform<GPU::AtomicBuffer> groupTotals;

    // How many elements there really are. A kernel that waits at a barrier has
    // no generated bounds guard - every thread must reach every barrier - so the
    // dispatch is rounded up to whole groups and this is what holds the stores
    // inside the array.
    GPU::Uniform<GPU::UInt> elementCount;

    EACP_SHADER(counts, offsets, groupTotals, elementCount)
};

// What the level above found, added back down: every element of a group takes
// the total of every group before it.
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

// The levels, and the buffers between them. Kept between frames like everything
// else a batch owns, so a canvas whose paths all move allocates nothing after
// its first frame.
class PrefixSum
{
public:
    PrefixSum() = default;

    // Records the sum of counts[0, count) into offsets, and leaves counts
    // zeroed. Both buffers belong to the caller and must hold at least count
    // unsigned integers; everything between the levels belongs to this.
    void run(GPU::ComputePass& pass,
             const GPU::Buffer& counts,
             const GPU::Buffer& offsets,
             int count);

    // Dispatches the last run took, which is two per level less one. It is what
    // a batch adds up to say what a frame costs.
    int getDispatchCount() const { return dispatches; }

private:
    // One rung: how many elements it sums, how many groups that is, the total
    // per group it leaves for the rung above, and - above the first - somewhere
    // to put its own scanned offsets.
    struct Level
    {
        int count = 0;
        int groups = 0;
        std::optional<GPU::Buffer> totals;
        std::optional<GPU::Buffer> offsets;
    };

    // Each rung is a thousandth of the one below it, so four of them reach a
    // million million elements. Fixed rather than grown because a level owns
    // buffers and is therefore not a thing to move around.
    static constexpr int maxLevels = 4;

    std::array<Level, maxLevels> levels;
    int levelCount = 0;
    int dispatches = 0;
};
} // namespace eacp::GPUWidgets
