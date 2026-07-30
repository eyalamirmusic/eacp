#pragma once

#include "CoverageKernel.h"

namespace eacp::GPUWidgets
{
// The three stages that build the array form of the backdrop, on the GPU.
//
// The backdrop is what everything to the left of a tile contributes to a pixel,
// and it comes in two forms - a run of steps per pixel row, or a value at every
// tile column of every pixel row. The second is priced by the *area*, and that
// was the single largest CPU cost of any path covering real area: a window-sized
// ellipse is two hundred and eighty-one segments and four hundred thousand
// cells, cleared, scattered into, summed and shipped every frame it moves.
//
// None of that has to happen on the CPU. What the CPU knows and the GPU does not
// is where the outline crosses into each tile column, and there are as many of
// those as there is outline - so the crossings go up and the array is built
// here, out of them:
//
//   clear    - one thread per cell, zeroing what the last dispatch left
//   scatter  - one thread per crossing, adding its winding to the rows it spans
//   scan     - one thread per pixel row, summing that row's cells left to right
//
// which is O(cells + crossings) rather than the O(cells x crossings) a per-pixel
// walk of the outline would be, and none of it crosses the bus.
//
// The cells are unsigned integers, because atomicAdd is the only way threads
// that never meet can accumulate into the same place and neither shader language
// has one for floats. See backdropFixedScale for what that costs, which is
// nothing that reaches a coverage step.

// Zeroes the cells this dispatch is about to scatter into. Only the cells the
// batch actually allocated: a canvas's array is megabytes, and clearing what
// nothing will read is the same waste on this side of the bus as it was on the
// other.
struct BackdropClearKernel final : GPU::ComputeProgram
{
    BackdropClearKernel() { compile(); }

    void define() override { write(cells, threadId(), 0u); }

    GPU::Uniform<GPU::AtomicBuffer> cells;

    EACP_SHADER(cells)
};

// One crossing of the outline into one tile column, over the part of one tile
// row's band it spans, added to every pixel row it covers.
//
// A crossing is three floats - the column, and the y it enters and leaves at,
// directed the way its own segment runs - so the sign of the winding is the sign
// of the span and is not a fourth float. It covers at most a band's sixteen
// rows, which is what bounds the loop.
struct BackdropScatterKernel final : PathIndexedKernel
{
    BackdropScatterKernel() { compile(); }

    void define() override
    {
        auto item = threadId();
        auto path = pathAt(item);

        auto shape = recordShape(path);
        auto crossing = crossings.read3(item);
        auto height = toUInt(shape.z());

        auto enters = crossing.y();
        auto leaves = crossing.z();

        // Everything the loop re-reads, read once. A value that is only an
        // expression is emitted again in every block that mentions it, so the
        // condition and the body would each fetch the record and the crossing -
        // nine loads around an add. See CoverageKernel::coverageAt, which holds
        // its tile offsets the same way and for the same reason.
        auto fromY = var(min(enters, leaves));
        auto toY = var(max(enters, leaves));
        auto columnBase = var(toUInt(shape.x()) + toUInt(crossing.x()) * height);

        // The direction and the fixed-point scale are one number: the sign of
        // the span is the sign of the winding, and the row's height is all that
        // is left to multiply by.
        auto winding =
            var(select(leaves > enters, backdropFixedScale, -backdropFixedScale));

        auto row = var(max(toInt(floor(fromY.get())), 0));
        auto last = var(min(toInt(ceil(toY.get())), toInt(height)));

        loop(row < last,
             [&]
             {
                 auto top = max(fromY.get(), toFloat(row.get()));
                 auto bottom = min(toY.get(), toFloat(row.get() + 1));
                 auto covered = var(bottom - top);

                 ifThen(covered > 0.f,
                        [&]
                        {
                            auto scaled = covered.get() * winding.get() + 0.5f;
                            atomicAdd(cells,
                                      columnBase.get() + toUInt(row.get()),
                                      toUInt(toInt(floor(scaled))));
                        });

                 row += 1;
             });
    }

    // Every path's crossings end to end, three floats each, in the order they
    // were binned. Which path a thread's crossing belongs to is what pathStarts
    // says; a path built as steps instead has no crossings here and an empty
    // run.
    GPU::Uniform<GPU::InputBuffer> crossings;
    GPU::Uniform<GPU::AtomicBuffer> cells;

    EACP_SHADER(crossings, records, cells, pathStarts, pathCount)
};

// One pixel row's cells, summed left to right in place: what crossed into a
// column becomes the winding entering it.
//
// A thread per row rather than a group per row. The rows are the parallelism -
// a canvas is tens of thousands of them - and a thread walking its row column by
// column reads a cell its neighbours' cells sit next to, the array being stored
// a column at a time. A group cooperating on one row would buy a shorter
// dependency chain and lose that, on the only paths that take this form at all.
struct BackdropScanKernel final : PathIndexedKernel
{
    BackdropScanKernel() { compile(); }

    void define() override
    {
        auto item = threadId();
        auto path = pathAt(item);

        auto shape = recordShape(path);

        // In locals, or the loop condition alone re-reads all four floats of the
        // record on every column.
        auto cellBase = var(toUInt(shape.x()));
        auto height = var(toUInt(shape.z()));
        auto tilesWide = var(toUInt(shape.w()));
        auto row = var(item - toUInt(pathStarts[path]));

        auto running = var(0u);
        auto column = var(0u);

        loop(column < tilesWide.get(),
             [&]
             {
                 auto cell =
                     cellBase.get() + column.get() * height.get() + row.get();

                 // Wrapping addition on the same two's-complement bits the
                 // scatter added in, so a negative winding sums as a negative
                 // one without either stage ever spelling a sign.
                 running += cells.load(cell);
                 write(cells, cell, running.get());

                 column += 1u;
             });
    }

    GPU::Uniform<GPU::AtomicBuffer> cells;

    EACP_SHADER(records, cells, pathStarts, pathCount)
};
} // namespace eacp::GPUWidgets
