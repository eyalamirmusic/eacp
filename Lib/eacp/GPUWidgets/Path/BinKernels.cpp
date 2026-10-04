#include "BinKernels.h"

namespace eacp::GPUWidgets
{
BinKernel::BinKernel()
{
    compile();
}

void BinKernel::define()
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
    // value in every block that mentions it, and this one is mentioned in the
    // innermost loop of all - so unpinned it is four loads per entry written
    // rather than four per segment. See CoverageKernel::coverageAt, which holds
    // its tile offsets the same way and for the same reason.
    auto segment = var(segments.read4(item));

    auto topY = var(min(segment.get().y(), segment.get().w()));
    auto bottomY = var(max(segment.get().y(), segment.get().w()));
    auto slope = var((segment.get().z() - segment.get().x())
                     / (segment.get().w() - segment.get().y()));

    // The direction and the fixed-point scale are one number: the sign of the
    // winding a crossing carries is the sign of its own segment, and the
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

             ifThen(bandBottom.get() > bandTop.get(),
                    [&]
                    {
                        auto enters = xAt(segment.get(), bandTop.get(), slope.get());
                        auto leaves =
                            xAt(segment.get(), bandBottom.get(), slope.get());

                        // The first column entirely to the right of the segment
                        // within this band. Everything from there on is backdrop,
                        // and everything before it is a list.
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

GPU::Int BinKernel::tileOf(const GPU::Float& coordinate)
{
    return toInt(floor(coordinate * (1.f / tileEdge)));
}

GPU::Int BinKernel::tileAfter(const GPU::Float& coordinate)
{
    return toInt(ceil(coordinate * (1.f / tileEdge)));
}

GPU::Float BinKernel::xAt(const GPU::Float4& segment,
                          const GPU::Float& y,
                          const GPU::Float& slope)
{
    return select(
        y == segment.w(), segment.z(), segment.x() + (y - segment.y()) * slope);
}

void BinKernel::addCrossing(const GPU::UInt& cellBase,
                            const GPU::UInt& height,
                            const GPU::UInt& tilesWide,
                            const GPU::UInt& column,
                            const GPU::Float& fromY,
                            const GPU::Float& toY,
                            const GPU::Float& winding)
{
    ifThen(column < tilesWide,
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

void BinKernel::fileUnder(const GPU::UInt& tile, const GPU::Float4& segment)
{
    ifThen(
        mode == countMode,
        [&] { atomicAdd(tileCounts, tile, 1u); },
        [&]
        {
            auto at = var(tileOffsets.load(tile) + atomicAdd(tileCounts, tile, 1u));

            ifThen(at.get() < entryCapacity,
                   [&] { write(tileSegments, at.get(), segment); });
        });
}

ClearKernel::ClearKernel()
{
    compile();
}

void ClearKernel::define()
{
    auto at = threadId();

    ifThen(at < cellCount, [&] { write(cells, at, 0u); });
    ifThen(at < tileCount, [&] { write(tileCounts, at, 0u); });
}
} // namespace eacp::GPUWidgets
