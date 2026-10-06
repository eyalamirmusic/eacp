#include "BackdropKernels.h"

namespace eacp::GPUWidgets
{
BackdropScanKernel::BackdropScanKernel()
{
    compile();
}

void BackdropScanKernel::define()
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
             auto cell = cellBase.get() + column.get() * height.get() + row.get();

             // Wrapping addition on the same two's-complement bits the binner
             // added in, so a negative winding sums as a negative one without
             // either stage ever spelling a sign.
             running += cells.load(cell);
             write(cells, cell, running.get());

             column += 1u;
         });
}
} // namespace eacp::GPUWidgets
