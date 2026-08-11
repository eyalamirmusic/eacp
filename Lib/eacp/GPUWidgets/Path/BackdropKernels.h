#pragma once

#include "CoverageKernel.h"

namespace eacp::GPUWidgets
{
// Sums one pixel row's cells left to right in place, turning the crossings the
// binner scattered into the winding entering each tile column. A thread per row,
// not a group: neighbouring threads then read neighbouring column-major cells.
struct BackdropScanKernel final : PathIndexedKernel
{
    BackdropScanKernel() { compile(); }

    void define() override
    {
        auto item = threadId();
        auto path = pathAt(item);

        auto shape = recordShape(path);

        // In locals, or the loop condition re-reads the whole record per column.
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

                 // Wrapping addition on the binner's two's-complement bits, so a
                 // negative winding sums as one without either stage signing it.
                 running += cells.load(cell);
                 write(cells, cell, running.get());

                 column += 1u;
             });
    }

    GPU::Uniform<GPU::AtomicBuffer> cells;

    EACP_SHADER(records, cells, pathStarts, pathCount)
};
} // namespace eacp::GPUWidgets
