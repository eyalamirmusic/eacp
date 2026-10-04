#pragma once

#include "../Common.h"

#include <eacp/GPU/Buffer/Buffer.h>

#include <optional>

namespace eacp::GPU
{
class ComputePass;
}

namespace eacp::GPUWidgets
{
// An exclusive prefix sum over a buffer of unsigned integers, on the GPU: the
// levels, and the buffers between them. The kernels it dispatches, and how the
// levels divide the work, are in PrefixSumKernels.h. Kept between frames like
// everything else a batch owns, so a canvas whose paths all move allocates
// nothing after its first frame.
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
    constexpr int getDispatchCount() const { return dispatches; }

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

    Array<Level, maxLevels> levels;
    int levelCount = 0;
    int dispatches = 0;
};
} // namespace eacp::GPUWidgets
