#include "CoverageKernel.h"

namespace eacp::GPUWidgets
{
GPU::UInt PathIndexedKernel::pathAt(const GPU::UInt& item)
{
    auto wanted = toFloat(item);
    auto lo = var(0);
    auto hi = var(pathCount);

    loop(lo < hi,
         [&]
         {
             auto mid = (lo.get() + hi.get()) >> 1;

             ifThen(
                 pathStarts[toUInt(mid)] <= wanted,
                 [&] { lo = mid + 1; },
                 [&] { hi = mid; });
         });

    // lo is the first run starting past this item, so the one before it owns
    // it. Stepped back with a clamp rather than a subtraction, the first entry
    // being zero and therefore never past anything.
    return toUInt(max(lo.get(), 1) - 1);
}

GPU::Float4 PathIndexedKernel::recordShape(const GPU::UInt& path)
{
    return records.read4(recordAt(path));
}

GPU::Float4 PathIndexedKernel::recordPlace(const GPU::UInt& path)
{
    return records.read4(recordAt(path) + 1u);
}

GPU::UInt PathIndexedKernel::tilesWideOf(const GPU::UInt& width)
{
    return (width + (unsigned) (tileSize - 1)) / (unsigned) tileSize;
}

GPU::UInt PathIndexedKernel::recordAt(const GPU::UInt& path)
{
    return path * (unsigned) (recordFloats / 4);
}

CoverageKernel::CoverageKernel()
{
    compile();
}

void CoverageKernel::define()
{
    // A block *is* a threadgroup, so which block this thread is in comes from
    // the group's own index rather than from dividing the thread's.
    //
    // That is not a tidiness: everything between here and the loop -- the
    // search, the twelve floats of the record, the two divisions by the path's
    // block width -- has one answer per group. Asked through the thread id it
    // is a dozen memory loads per thread that a compiler cannot prove uniform;
    // asked through the group id it is provably uniform and the hardware
    // answers it once for the whole group. On a path with half a segment test
    // per pixel that difference was the rasterization several times over.
    auto group = groupPosition();
    auto lane = localPosition();

    auto block = group.y * gridColumns + group.x;

    auto path = pathAt(block);

    auto shape = recordShape(path);
    auto place = recordPlace(path);

    auto cellBase = toUInt(shape.x());
    auto width = toUInt(shape.y());
    auto height = toUInt(shape.z());
    auto tileBase = toUInt(shape.w());

    auto originX = toUInt(place.y());
    auto originY = toUInt(place.z());

    auto blocksWide = (width + (unsigned) (blockSize - 1)) / (unsigned) blockSize;

    // Where in its own path this thread's pixel is. A block past the end of the
    // batch belongs to the last path and lands well below it, which is what the
    // guard below retires - so the search never needs its own.
    auto local = block - toUInt(pathStarts[path]);
    auto pixelX = (local % blocksWide) * (unsigned) blockSize + lane.x;
    auto pixelY = (local / blocksWide) * (unsigned) blockSize + lane.y;

    ifThen(pixelX < width && pixelY < height,
           [&]
           {
               auto value = coverageAt(pixelX,
                                       pixelY,
                                       tileBase,
                                       cellBase,
                                       height,
                                       tilesWideOf(width),
                                       place.x());

               // The same coverage in all four channels. A one-channel mask is
               // what this is, but R8Unorm is outside the set a typed UAV store
               // is guaranteed for - see supportsComputeWrite - so the texture
               // is RGBA8 and whoever samples it reads whichever channel it
               // likes.
               //
               // The origin is what lets several paths share one texture: the
               // segments arrive in each path's own space, so only the write
               // moves.
               write(coverage,
                     pixelX + originX,
                     pixelY + originY,
                     float4(value, value, value, value));
           });
}

GPU::Float CoverageKernel::coverageAt(const GPU::UInt& pixelX,
                                      const GPU::UInt& pixelY,
                                      const GPU::UInt& tileBase,
                                      const GPU::UInt& cellBase,
                                      const GPU::UInt& height,
                                      const GPU::UInt& tilesWide,
                                      const GPU::Float& evenOdd)
{
    auto x = toFloat(pixelX);
    auto y = toFloat(pixelY);

    auto column = pixelX / (unsigned) tileSize;
    auto tile = tileBase + (pixelY / (unsigned) tileSize) * tilesWide + column;

    // Everything left of this tile covers the pixel's whole row-slice, so its
    // contribution depends on the row and not on the column - which is what
    // lets it arrive as a number the thread starts from instead of a list it
    // walks. One load, the stages ahead of this one having already summed it
    // along the row.
    auto winding = var(cellAt(cellBase + column * height + pixelY));

    // Held in locals rather than re-read: the loop condition is re-tested in
    // the generated while header, and these do not change under it.
    //
    // The offsets are the batch's own, not the path's: the prefix sum that
    // produced them ran over every tile of every path at once, so a tile's run
    // is already where it is in the one segment array and nothing has a base
    // to add.
    auto index = var(tileOffsets.load(tile));
    auto last = var(tileOffsets.load(tile + 1u));

    loop(index.get() < last.get(),
         [&]
         {
             auto segment = tileSegments.read4(index.get());

             auto ax = segment.x() - x;
             auto ay = segment.y() - y;
             auto bx = segment.z() - x;
             auto by = segment.w() - y;

             // The part of the segment's vertical span that falls inside this
             // pixel. Zero for a segment above it, below it, or horizontal -
             // which is the one guard the body needs, since a horizontal
             // segment is also the only one whose slope divides by zero below.
             auto low = clamp(min(ay, by), 0.f, 1.f);
             auto high = clamp(max(ay, by), 0.f, 1.f);
             auto height = high - low;

             ifThen(height > 0.f,
                    [&]
                    {
                        // Where the segment enters and leaves that span.
                        auto slope = 1.f / (by - ay);
                        auto xLow = ax + (low - ay) * slope * (bx - ax);
                        auto xHigh = ax + (high - ay) * slope * (bx - ax);

                        auto direction = select(by > ay, height, -height);
                        winding += direction * (1.f - meanClampedX(xLow, xHigh));
                    });

             index += 1u;
         });

    auto total = abs(winding.get());

    // Even-odd folds the winding into a triangle wave - 0, 1, 0, 1 as it rises
    // - so a doubly-wound region reads as a hole; non-zero simply saturates.
    // Both are computed and one is chosen, rather than branched on: the choice
    // is uniform across a whole path, so a branch here would buy nothing and
    // cost the divergence check.
    auto folded = fract(total * 0.5f) * 2.f;
    auto evenOddCoverage = min(folded, 2.f - folded);
    auto nonZeroCoverage = min(total, 1.f);

    return select(evenOdd != 0.f, evenOddCoverage, nonZeroCoverage);
}

GPU::Float CoverageKernel::cellAt(const GPU::UInt& index)
{
    return toFloat(toInt(cells.load(index))) * (1.f / backdropFixedScale);
}

GPU::Float CoverageKernel::clampedIntegral(const GPU::Float& x)
{
    auto inside = clamp(x, 0.f, 1.f);
    return inside * inside * 0.5f + max(x - 1.f, 0.f);
}

GPU::Float CoverageKernel::meanClampedX(const GPU::Float& from, const GPU::Float& to)
{
    auto run = to - from;
    auto flat = abs(run) < 1e-6f;

    auto ramp =
        (clampedIntegral(to) - clampedIntegral(from)) / select(flat, 1.f, run);

    return select(flat, clamp(from, 0.f, 1.f), ramp);
}
} // namespace eacp::GPUWidgets
