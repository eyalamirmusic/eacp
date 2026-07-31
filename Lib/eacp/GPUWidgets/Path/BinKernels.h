#pragma once

#include "BackdropKernels.h"
#include "PrefixSum.h"

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

    BinKernel() { compile(); }

    void define() override
    {
        auto item = threadId();
        auto path = pathAt(item);
        auto shape = recordShape(path);

        auto cellBase = var(toUInt(shape.x()));
        auto height = var(toUInt(shape.z()));
        auto tilesWide = var(tilesWideOf(toUInt(shape.y())));
        auto tilesHigh = var(tilesWideOf(height.get()));
        auto tileBase = var(toUInt(shape.w()));

        // Pinned to a local, not left as an expression: the emitter re-emits a
        // value in every block that mentions it, and this one is mentioned in
        // the innermost loop of all - so unpinned it is four loads per entry
        // written rather than four per segment. See CoverageKernel::coverageAt,
        // which holds its tile offsets the same way and for the same reason.
        auto segment = var(segments.read4(item));

        auto fromX = var(segment.get().x());
        auto fromY = var(segment.get().y());
        auto topY = var(min(segment.get().y(), segment.get().w()));
        auto bottomY = var(max(segment.get().y(), segment.get().w()));
        auto slope = var((segment.get().z() - segment.get().x())
                         / (segment.get().w() - segment.get().y()));

        // The direction and the fixed-point scale are one number: the sign of
        // the winding a crossing carries is the sign of its own segment, and the
        // covered height is all that is left to multiply by.
        auto winding = var(select(segment.get().w() > segment.get().y(),
                                  backdropFixedScale,
                                  -backdropFixedScale));

        auto row = var(max(tileOf(topY.get()), 0));
        auto lastRow =
            var(min(tileAfter(bottomY.get()) - 1, toInt(tilesHigh.get()) - 1));

        loop(row.get() <= lastRow.get(),
             [&]
             {
                 auto bandTop = var(max(topY.get(), toFloat(row.get()) * tileEdge));
                 auto bandBottom =
                     var(min(bottomY.get(), toFloat(row.get() + 1) * tileEdge));

                 ifThen(
                     bandBottom.get() > bandTop.get(),
                     [&]
                     {
                         auto enters = fromX.get()
                                       + (bandTop.get() - fromY.get()) * slope.get();
                         auto leaves =
                             fromX.get()
                             + (bandBottom.get() - fromY.get()) * slope.get();

                         // The first column entirely to the right of the
                         // segment within this band. Everything from there on
                         // is backdrop, and everything before it is a list.
                         auto beyond = var(max(tileAfter(max(enters, leaves)), 0));

                         ifThen(mode == countMode,
                                [&]
                                {
                                    addCrossing(cellBase.get(),
                                                height.get(),
                                                tilesWide.get(),
                                                toUInt(beyond.get()),
                                                bandTop.get(),
                                                bandBottom.get(),
                                                winding.get());
                                });

                         auto column = var(max(tileOf(min(enters, leaves)), 0));
                         auto lastColumn =
                             var(min(beyond.get() - 1, toInt(tilesWide.get()) - 1));

                         loop(column.get() <= lastColumn.get(),
                              [&]
                              {
                                  auto tile = tileBase.get()
                                              + toUInt(row.get()) * tilesWide.get()
                                              + toUInt(column.get());

                                  fileUnder(tile, segment.get());
                                  column += 1;
                              });
                     });

                 row += 1;
             });
    }

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
    static GPU::Int tileOf(const GPU::Float& coordinate)
    {
        return toInt(floor(coordinate * (1.f / tileEdge)));
    }

    static GPU::Int tileAfter(const GPU::Float& coordinate)
    {
        return toInt(ceil(coordinate * (1.f / tileEdge)));
    }

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
                     const GPU::Float& winding)
    {
        ifThen(
            column < tilesWide,
            [&]
            {
                auto columnBase = var(cellBase + column * height);
                auto top = var(fromY);
                auto bottom = var(toY);

                auto row = var(max(toInt(floor(top.get())), 0));
                auto last = var(min(toInt(ceil(bottom.get())), toInt(height)));

                loop(row.get() < last.get(),
                     [&]
                     {
                         auto rowTop = max(top.get(), toFloat(row.get()));
                         auto rowBottom = min(bottom.get(), toFloat(row.get() + 1));
                         auto covered = var(rowBottom - rowTop);

                         ifThen(covered.get() > 0.f,
                                [&]
                                {
                                    auto scaled = covered.get() * winding + 0.5f;
                                    atomicAdd(cells,
                                              columnBase.get() + toUInt(row.get()),
                                              toUInt(toInt(floor(scaled))));
                                });

                         row += 1;
                     });
            });
    }

    // A segment under one of its tiles: counted on the first pass, written on
    // the second. The cursor is the same array the counts were in - the prefix
    // sum leaves it zeroed behind itself, so what counted the entries is what
    // hands them out.
    void fileUnder(const GPU::UInt& tile, const GPU::Float4& segment)
    {
        ifThen(
            mode == countMode,
            [&] { atomicAdd(tileCounts, tile, 1u); },
            [&]
            {
                auto at =
                    var(tileOffsets.load(tile) + atomicAdd(tileCounts, tile, 1u));

                ifThen(at.get() < entryCapacity,
                       [&] { write(tileSegments, at.get(), segment); });
            });
    }
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
    ClearKernel() { compile(); }

    void define() override
    {
        auto at = threadId();

        ifThen(at < cellCount, [&] { write(cells, at, 0u); });
        ifThen(at < tileCount, [&] { write(tileCounts, at, 0u); });
    }

    GPU::Uniform<GPU::AtomicBuffer> cells;
    GPU::Uniform<GPU::AtomicBuffer> tileCounts;
    GPU::Uniform<GPU::UInt> cellCount;
    GPU::Uniform<GPU::UInt> tileCount;

    EACP_SHADER(cells, tileCounts, cellCount, tileCount)
};
} // namespace eacp::GPUWidgets
