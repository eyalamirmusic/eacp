#pragma once

#include "../Common.h"

#include <eacp/GPU/Codegen/Codegen.h>

namespace eacp::GPUWidgets
{
// An exclusive prefix sum over a buffer of unsigned integers, on the GPU - the
// two kernels PrefixSum dispatches.
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

    ScanBlockKernel();

    void define() override;

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
    ScanAddKernel();

    void define() override;

    GPU::Uniform<GPU::AtomicBuffer> offsets;
    GPU::Uniform<GPU::AtomicBuffer> groupOffsets;

    EACP_SHADER(offsets, groupOffsets)
};
} // namespace eacp::GPUWidgets
