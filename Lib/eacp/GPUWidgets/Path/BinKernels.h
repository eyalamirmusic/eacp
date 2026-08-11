#pragma once

#include "BackdropKernels.h"
#include "PrefixSum.h"

namespace eacp::GPUWidgets
{
// Sorts a batch's segments into tiles, clipping per tile row rather than by
// bounding box, and records the backdrop crossings from the same clip. Count and
// fill are one kernel dispatched twice, so their arithmetic cannot drift apart.
struct BinKernel final : PathIndexedKernel
{
    // Uniform across the dispatch, so every thread takes the same arm.
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

        // Pinned to a local: the emitter re-emits an expression in every block
        // mentioning it, and this one is read in the innermost loop.
        auto segment = var(segments.read4(item));

        auto fromX = var(segment.get().x());
        auto fromY = var(segment.get().y());
        auto topY = var(min(segment.get().y(), segment.get().w()));
        auto bottomY = var(max(segment.get().y(), segment.get().w()));
        auto slope = var((segment.get().z() - segment.get().x())
                         / (segment.get().w() - segment.get().y()));

        // Direction and fixed-point scale in one number; only the covered height
        // is left to multiply by.
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

                         // The first column right of the segment in this band:
                         // backdrop from there on, binned entries before it.
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

    // Every path's segments end to end, four floats each, in the pixel space of
    // the path they belong to.
    GPU::Uniform<GPU::InputBuffer> segments;

    // Integers, because an atomic add is.
    GPU::Uniform<GPU::AtomicBuffer> cells;
    GPU::Uniform<GPU::AtomicBuffer> tileCounts;

    // Meaningless until the prefix sum has run, so touched on the fill pass only.
    GPU::Uniform<GPU::AtomicBuffer> tileOffsets;
    GPU::Uniform<GPU::OutputBuffer> tileSegments;

    GPU::Uniform<GPU::UInt> mode;

    // Room for segment-tile entries, from a CPU-side bound. The guard makes a
    // short bound a missing segment rather than a write past the array.
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

    // [tileOf(from), tileAfter(to) - 1] is every tile a span touches.
    static GPU::Int tileOf(const GPU::Float& coordinate)
    {
        return toInt(floor(coordinate * (1.f / tileEdge)));
    }

    static GPU::Int tileAfter(const GPU::Float& coordinate)
    {
        return toInt(ceil(coordinate * (1.f / tileEdge)));
    }

    // One crossing into one tile column, added to every pixel row of the band it
    // spans - at most tileSize of them, which is what bounds the loop.
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

    // Counted on the first pass, written on the second. tileCounts doubles as
    // the cursor, the prefix sum having left it zeroed behind itself.
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

// Zeroes what the last dispatch left in the cells and the tile counts, only as
// far as this batch uses them. Two arrays in one dispatch: neither is read
// before the other is written.
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
