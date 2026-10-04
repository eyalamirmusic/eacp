#pragma once

#include "CoverageKernel.h"

namespace eacp::GPUWidgets
{
// Sorting a batch's segments into the tiles that walk them, on the GPU.
//
// A segment does not contribute only to the pixels it passes through: in this
// formulation it contributes the signed area to its right, so one anywhere to
// the left of a pixel adds its whole winding to it. Binning by overlap alone
// therefore loses everything to the right of the outline, and what carries it is
// the backdrop - see BackdropKernels.h. Both come out of the same clip, which is
// why the crossing is recorded here rather than in a stage of its own.
//
// The clip is per segment per tile row, not per bounding box. A long diagonal
// binned by its box lands in every tile of a square; clipped to each row it
// lands in the two or three per row it actually crosses, which is the difference
// between binning helping and binning being another way to do the same work.
//
//   clear  - one thread per cell and per tile, zeroing what the last frame left
//   count  - one thread per segment: the crossings, and a count per tile
//   sum    - PrefixSum, turning the counts into offsets and the counts into
//            cursors
//   fill   - the same threads again, writing each segment into each of its tiles
//
// **The count and the fill are one kernel, dispatched twice.** They have to
// agree exactly about which tiles a segment lands in - a fill that found one
// more tile than the count did would write past the end of that tile's run and
// into the next one's - and two kernels holding the same arithmetic are only
// equal until a shader compiler contracts a multiply-add in one of them and not
// the other. One kernel and a uniform mode is the only version of this that
// cannot drift, and the branch is uniform across the whole dispatch.
struct BinKernel final : PathIndexedKernel
{
    // Counting, or writing what was counted. A uniform, so every thread of every
    // group takes the same arm.
    static constexpr unsigned countMode = 0;
    static constexpr unsigned fillMode = 1;

    BinKernel();

    void define() override;

    // Every path's segments end to end, four floats each, in the coverage pixel
    // space of the path they belong to. Which path a thread's segment is in is
    // what pathStarts says, and its own index in the batch is the thread's.
    GPU::Uniform<GPU::InputBuffer> segments;

    // The backdrop the crossings accumulate into, and the count per tile the
    // prefix sum turns into offsets. Both are integers because an atomic add is.
    GPU::Uniform<GPU::AtomicBuffer> cells;
    GPU::Uniform<GPU::AtomicBuffer> tileCounts;

    // Where every tile's run begins, and the segments themselves. Read and
    // written only on the second pass; on the first the counts are not summed
    // yet and neither holds anything.
    GPU::Uniform<GPU::AtomicBuffer> tileOffsets;
    GPU::Uniform<GPU::OutputBuffer> tileSegments;

    GPU::Uniform<GPU::UInt> mode;

    // How many segment-tile entries there is room for. The count is not on this
    // side of the wire, so the array is sized to a bound taken per segment
    // without clipping anything - see PathRasterizer::measure. The guard is what
    // makes a bound that was somehow too small a missing segment rather than a
    // write into whatever follows.
    GPU::Uniform<GPU::UInt> entryCapacity;

    EACP_SHADER(segments,
                records,
                pathStarts,
                cells,
                tileCounts,
                tileOffsets,
                tileSegments,
                mode,
                entryCapacity,
                pathCount)

private:
    static constexpr float tileEdge = (float) tileSize;

    // The tile a coordinate falls in, and the first tile entirely past it.
    // Together they bracket a span, exactly as the pair of the same name on the
    // CPU did.
    static GPU::Int tileOf(const GPU::Float& coordinate);
    static GPU::Int tileAfter(const GPU::Float& coordinate);

    // Where the segment is at a height, with both ends exact. The start is
    // exact already - its offset is a zero - but the end is interpolated back
    // through the slope, and a division under fast math or a contracted
    // multiply-add lands an ulp either side of it. An end on a tile edge then
    // lists the segment in the next column and moves its crossing there, and
    // which way it lands is the device's business - so the end is taken as
    // itself rather than as a product.
    static GPU::Float xAt(const GPU::Float4& segment,
                          const GPU::Float& y,
                          const GPU::Float& slope);

    // One crossing of the outline into one tile column, added to every pixel row
    // of the band it spans. A band is sixteen rows at most, which is what bounds
    // the loop.
    //
    // This is the whole of the backdrop's scatter, and it lives inside the clip
    // rather than in a stage of its own because the clip is what produces it:
    // recording the crossings to a buffer and reading them back in a second
    // dispatch would be the same arithmetic twice and a buffer the size of the
    // outline in between.
    void addCrossing(const GPU::UInt& cellBase,
                     const GPU::UInt& height,
                     const GPU::UInt& tilesWide,
                     const GPU::UInt& column,
                     const GPU::Float& fromY,
                     const GPU::Float& toY,
                     const GPU::Float& winding);

    // A segment under one of its tiles: counted on the first pass, written on
    // the second. The cursor is the same array the counts were in - the prefix
    // sum leaves it zeroed behind itself, so what counted the entries is what
    // hands them out.
    void fileUnder(const GPU::UInt& tile, const GPU::Float4& segment);
};

// Zeroes what the last dispatch into these buffers left: the backdrop's cells,
// and the count per tile.
//
// Only what the batch actually uses. A canvas's arrays are megabytes, and
// clearing what nothing will read is the same waste on this side of the bus as
// it was on the other.
//
// One kernel for two arrays rather than two dispatches for two arrays. They are
// cleared at the same point for the same reason and neither is read before the
// other is written, so the only thing a second dispatch would buy is a second
// dispatch.
struct ClearKernel final : GPU::ComputeProgram
{
    ClearKernel();

    void define() override;

    GPU::Uniform<GPU::AtomicBuffer> cells;
    GPU::Uniform<GPU::AtomicBuffer> tileCounts;
    GPU::Uniform<GPU::UInt> cellCount;
    GPU::Uniform<GPU::UInt> tileCount;

    EACP_SHADER(cells, tileCounts, cellCount, tileCount)
};
} // namespace eacp::GPUWidgets
