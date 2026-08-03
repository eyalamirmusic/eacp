#pragma once

#include "CoverageKernel.h"

namespace eacp::GPUWidgets
{
// The backdrop: what everything to the left of a tile contributes to a pixel,
// held as a value at every tile column of every pixel row.
//
// It is priced by the *area*, and that was the single largest CPU cost of any
// path covering real area - a window-sized ellipse is two hundred and eighty-one
// segments and four hundred thousand cells, cleared, scattered into, summed and
// shipped every frame it moves.
//
// None of it happens there now. Where the outline crosses into each tile column
// falls out of the same clip that decides which tiles a segment lands in, so the
// binning kernel adds each crossing to the rows it covers as it goes - see
// BinKernels.h - and what is left is this: the running sum along each pixel row
// that turns what crossed into a column into the winding entering it.
//
// The cells are unsigned integers, because atomicAdd is the only way threads
// that never meet can accumulate into the same place and neither shader language
// has one for floats. See backdropFixedScale for what that costs, which is
// nothing that reaches a coverage step.

// One pixel row's cells, summed left to right in place.
//
// A thread per row rather than a group per row. The rows are the parallelism -
// a canvas is tens of thousands of them - and a thread walking its row column by
// column reads a cell its neighbours' cells sit next to, the array being stored
// a column at a time. A group cooperating on one row would buy a shorter
// dependency chain and lose that.
//
// It does bite on a single wide path, which has few rows and many columns: an
// automation curve is 356 rows of 151, so six threadgroups each walking 151
// dependent loads, and cutting the walk to one column takes the stage from 0.207
// to 0.090ms. On a canvas it does not - 128 lanes is 45,568 rows - and a
// log-depth scan does several times the work of a serial one, so buying the solo
// case would cost the case this exists for.
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
        auto tilesWide = var(tilesWideOf(toUInt(shape.y())));
        auto row = var(item - toUInt(pathStarts[path]));

        auto running = var(0u);
        auto column = var(0u);

        loop(column < tilesWide.get(),
             [&]
             {
                 auto cell =
                     cellBase.get() + column.get() * height.get() + row.get();

                 // Wrapping addition on the same two's-complement bits the
                 // binner added in, so a negative winding sums as a negative
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
