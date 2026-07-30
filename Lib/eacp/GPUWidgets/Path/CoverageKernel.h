#pragma once

#include "../Common.h"

namespace eacp::GPUWidgets
{
// The backdrop is accumulated by an atomic add, and neither shader language has
// one for floats - so the winding is carried as a fixed-point integer while it
// is being summed, and crosses back to a float when it is read. Twenty
// fractional bits leaves eleven for the whole part, which is a winding depth of
// two thousand: past any outline this rasterizer will see, and fine enough that
// a row summing a thousand crossings is still inside a thousandth of a coverage
// step.
constexpr float backdropFixedScale = 1048576.f;

// A dispatch whose work belongs to many paths, and the search that says which.
// Every stage of a batched rasterization needs it - a thread has an item index
// and nothing else - and they need it identically, so it is written once here.
struct PathIndexedKernel : GPU::ComputeProgram
{
    // Which path owns a work item: the last one whose run starts at or before
    // it. Binary search rather than a walk because a canvas is a hundred paths
    // and this runs once per item.
    //
    // A path with nothing for this stage has a run of length zero, and the
    // search steps over it rather than landing on it: its start is the same
    // number as its successor's, so the first start *past* the item is past
    // both, and the entry before that is the one that owns it.
    GPU::UInt pathAt(const GPU::UInt& item)
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

        // lo is the first run starting past this item, so the one before it
        // owns it. Stepped back with a clamp rather than a subtraction, the
        // first entry being zero and therefore never past anything.
        return toUInt(max(lo.get(), 1) - 1);
    }

    // Floats per path record: three float4 reads, and every field of them used.
    // How wide the path's block grid is would be a thirteenth and is derived
    // from the width instead, which is a shift and an add against a fourth read
    // and four floats per path.
    static constexpr int recordFloats = 12;

    // The three reads a record is, named rather than counted. Every stage wants
    // a different one of them and a stage that wanted the wrong one would read
    // plausible numbers out of the neighbouring field - so the offsets are
    // written once here, beside the constant, rather than spelled at each call.
    GPU::Float4 recordBases(const GPU::UInt& path)
    {
        return records.read4(recordAt(path));
    }

    GPU::Float4 recordShape(const GPU::UInt& path)
    {
        return records.read4(recordAt(path) + 1u);
    }

    GPU::Float4 recordPlace(const GPU::UInt& path)
    {
        return records.read4(recordAt(path) + 2u);
    }

    // Where each path's run of this stage's items starts, with a last entry
    // holding the total. The one buffer a thread reads before it knows anything
    // at all, which is why it is its own buffer rather than a field of the
    // records: the search wants its keys next to each other.
    GPU::Uniform<GPU::InputBuffer> pathStarts;

    // Twelve floats per path: where its runs begin in each of the buffers, how
    // big its coverage is, how wide its tile grid is, where it sits in the
    // target, and which fill rule and backdrop form it was built for. Read by
    // every stage, through the three accessors above.
    GPU::Uniform<GPU::InputBuffer> records;

    // How many paths the search is over. Not the run total: the last entry of
    // pathStarts is a terminator and belongs to no path.
    GPU::Uniform<GPU::Int> pathCount;

private:
    static GPU::UInt recordAt(const GPU::UInt& path)
    {
        return path * (unsigned) (recordFloats / 4);
    }
};

// The EDSL-authored kernel behind PathRasterizer: one thread per pixel,
// accumulating each segment's signed contribution to its own pixel's coverage.
//
// It is the trapezoid arithmetic a scanline cell rasterizer (FreeType's, and
// every one since) does, evaluated per pixel instead of accumulated along a row.
// Working in pixel-local coordinates - where the thread's own pixel is the unit
// box - a segment crossing the box contributes the signed area to the right of
// it: the vertical extent it spans inside the box, times how much of the box lies
// to its right. Summed over a closed contour, that *is* the winding number where
// the edge misses the pixel entirely and the exact fractional area where it does
// not, which is why this antialiases without sampling anything.
//
// Exact, not approximate: see meanClampedX below for the one place where that
// costs anything, and for what sampling instead would get wrong.
//
// A thread walks only the segments of its own tile. The rest of the path reaches
// it as a single number - the winding entering the tile from the left, at the
// thread's own pixel row - so a pixel pays for the outline passing near it and
// not for the outline existing. See PathRasterizer, which does the binning and
// the transform into pixel space, and BackdropKernels.h for where that number
// comes from when it is not a list of steps.
//
// Horizontal segments contribute nothing, and are dropped on the CPU rather than
// guarded against here.
//
// **One dispatch rasterizes many paths.** Every buffer below holds every path in
// the batch end to end, and a record says where each one's run starts; a thread
// finds which path it is working on before it does anything else. See
// CoverageBatch, which is what concatenates them. A single path is a batch of
// one and goes down exactly the same road.
struct CoverageKernel final : PathIndexedKernel
{
    // Coverage pixels along one side of a tile. A thread reads two offsets and a
    // backdrop before its loop, so tiles want to be big enough that the three
    // reads disappear against the segments they save; they also want to be small
    // enough that a tile holds only the outline near it. Sixteen is four of the
    // 8x8 threadgroups the 2D dispatch uses, so the offsets a group reads are
    // the same for every thread in it.
    static constexpr int tileSize = 16;

    // The side of one dispatch block, which is the 2D threadgroup's own shape.
    // Paths are laid out in the grid a block at a time rather than a pixel at a
    // time, so a group's 64 threads are the 8x8 corner of exactly one path - the
    // locality the per-pixel dispatch had, kept across a batch that has no
    // rectangle to dispatch over.
    static constexpr int blockSize = GPU::ComputeProgram::groupSize2D;

    CoverageKernel() { compile(); }

    void define() override
    {
        // A block *is* a threadgroup, so which block this thread is in comes
        // from the group's own index rather than from dividing the thread's.
        //
        // That is not a tidiness: everything between here and the loop -- the
        // search, the twelve floats of the record, the two divisions by the
        // path's block width -- has one answer per group. Asked through the
        // thread id it is a dozen memory loads per thread that a compiler
        // cannot prove uniform; asked through the group id it is provably
        // uniform and the hardware answers it once for the whole group. On a
        // path with half a segment test per pixel that difference was the
        // rasterization several times over.
        auto group = groupPosition();
        auto lane = localPosition();

        auto block = group.y * gridColumns + group.x;

        auto path = pathAt(block);

        auto bases = recordBases(path);
        auto shape = recordShape(path);
        auto place = recordPlace(path);

        auto segmentBase = toUInt(bases.x());
        auto tileBase = toUInt(bases.y());
        auto stepBase = toUInt(bases.z());
        auto rowBase = toUInt(bases.w());

        auto denseBase = toUInt(shape.x());
        auto width = toUInt(shape.y());
        auto height = toUInt(shape.z());
        auto tilesWide = toUInt(shape.w());

        auto originX = toUInt(place.x());
        auto originY = toUInt(place.y());
        auto blocksWide =
            (width + (unsigned) (blockSize - 1)) / (unsigned) blockSize;

        // Where in its own path this thread's pixel is. A block past the end of
        // the batch belongs to the last path and lands well below it, which is
        // what the guard below retires - so the search never needs its own.
        auto local = block - toUInt(pathStarts[path]);
        auto pixelX = (local % blocksWide) * (unsigned) blockSize + lane.x;
        auto pixelY = (local / blocksWide) * (unsigned) blockSize + lane.y;

        ifThen(pixelX < width && pixelY < height,
               [&]
               {
                   auto value = coverageAt(pixelX,
                                           pixelY,
                                           segmentBase,
                                           tileBase,
                                           stepBase,
                                           rowBase,
                                           denseBase,
                                           height,
                                           tilesWide,
                                           place.z(),
                                           place.w());

                   // The same coverage in all four channels. A one-channel mask
                   // is what this is, but R8Unorm is outside the set a typed UAV
                   // store is guaranteed for - see supportsComputeWrite - so the
                   // texture is RGBA8 and whoever samples it reads whichever
                   // channel it likes.
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

    // One pixel's coverage, in its own path's space. Everything it needs to find
    // its way into the shared buffers arrives as a base, so the arithmetic below
    // is the single-path arithmetic it always was.
    GPU::Float coverageAt(const GPU::UInt& pixelX,
                          const GPU::UInt& pixelY,
                          const GPU::UInt& segmentBase,
                          const GPU::UInt& tileBase,
                          const GPU::UInt& stepBase,
                          const GPU::UInt& rowBase,
                          const GPU::UInt& denseBase,
                          const GPU::UInt& height,
                          const GPU::UInt& tilesWide,
                          const GPU::Float& evenOdd,
                          const GPU::Float& sparse)
    {
        auto x = toFloat(pixelX);
        auto y = toFloat(pixelY);

        auto column = pixelX / (unsigned) tileSize;
        auto tile = tileBase + (pixelY / (unsigned) tileSize) * tilesWide + column;

        // Everything left of this tile covers the pixel's whole row-slice, so
        // its contribution depends on the row and not on the column - which is
        // what lets it arrive as a number the thread starts from instead of a
        // list it walks.
        //
        // Which of the two forms it arrives in is settled per path, so this
        // branch is taken one way by every thread of a path and costs nothing
        // beyond being written twice.
        auto winding = var(0.f);

        ifThen(
            sparse != 0.f,
            [&]
            { winding = backdropAt(rowBase + pixelY, stepBase, toFloat(column)); },
            [&] { winding = cellAt(denseBase + column * height + pixelY); });

        // Held in locals rather than re-read: the loop condition is re-tested in
        // the generated while header, and these do not change under it.
        auto index = var(toInt(tileOffsets[tile]));
        auto last = var(toInt(tileOffsets[tile + 1u]));

        loop(index < last,
             [&]
             {
                 auto segment = segments.read4(segmentBase + toUInt(index));

                 auto ax = segment.x() - x;
                 auto ay = segment.y() - y;
                 auto bx = segment.z() - x;
                 auto by = segment.w() - y;

                 // The part of the segment's vertical span that falls inside
                 // this pixel. Zero for a segment above it, below it, or
                 // horizontal - which is the one guard the body needs, since a
                 // horizontal segment is also the only one whose slope divides
                 // by zero below.
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

                 index += 1;
             });

        auto total = abs(winding.get());

        // Even-odd folds the winding into a triangle wave - 0, 1, 0, 1 as it
        // rises - so a doubly-wound region reads as a hole; non-zero simply
        // saturates. Both are computed and one is chosen, rather than branched
        // on: the choice is uniform across a whole path, so a branch here would
        // buy nothing and cost the divergence check.
        auto folded = fract(total * 0.5f) * 2.f;
        auto evenOddCoverage = min(folded, 2.f - folded);
        auto nonZeroCoverage = min(total, 1.f);

        return select(evenOdd != 0.f, evenOddCoverage, nonZeroCoverage);
    }

    // The same thing when it was built as an array: one cell per tile column per
    // pixel row, already summed left to right by the scan stage, so the lookup
    // is the read and nothing else.
    //
    // The cells are integers because an atomic add is - see backdropFixedScale -
    // so this is where they stop being. Column-major, which is not the order it
    // reads in: it is the order the scan writes in, and a scan thread owns a
    // row while a coverage block owns eight of them at one column, so both walk
    // consecutive cells and only the scatter is scattered.
    GPU::Float cellAt(const GPU::UInt& index)
    {
        return toFloat(toInt(cells.load(index))) * (1.f / backdropFixedScale);
    }

    // The winding entering this tile column, at this pixel row: everything the
    // outline does to the left of here, which the thread would otherwise have to
    // walk the whole of.
    //
    // A row's backdrop is a step function, and its steps are the outline's
    // crossings of that row rather than the row's tile columns - so what is
    // stored is the steps, and the lookup is for the last one at or left of this
    // column. Binary search rather than a walk because the two are priced
    // differently in the case that matters: dense artwork puts a hundred steps
    // in a row, and this runs once per pixel.
    GPU::Float backdropAt(const GPU::UInt& row,
                          const GPU::UInt& stepBase,
                          const GPU::Float& column)
    {
        auto first = toInt(backdropRows[row]);
        auto lo = var(first);
        auto hi = var(toInt(backdropRows[row + 1u]));

        loop(lo < hi,
             [&]
             {
                 auto mid = (lo.get() + hi.get()) >> 1;

                 ifThen(
                     backdropSteps.read2(stepBase + toUInt(mid)).x() <= column,
                     [&] { lo = mid + 1; },
                     [&] { hi = mid; });
             });

        // lo is the first step past this column, so the one before it holds the
        // answer, and no step before it means nothing has crossed this row yet.
        // The read is taken either way, select having no unevaluated arm, so the
        // index is stepped back with a clamp rather than a subtraction - which
        // is what keeps it inside a buffer that always holds at least one step.
        auto found = backdropSteps.read2(stepBase + toUInt(max(lo.get(), 1) - 1));
        return select(lo > first, found.y(), 0.f);
    }

    // The mean of clamp(x, 0, 1) as x runs linearly from one value to the
    // other, which is the exact fraction of the pixel row-slice that lies to
    // the *left* of the segment. Sampling x at the span's midpoint instead
    // would be very close, and wrong in one specific way: where the segment
    // leaves through the pixel's left or right side it puts the whole ramp in
    // one pixel, when it belongs spread across two. That is a tenth of a
    // coverage step at 45 degrees, and it is what makes an edge read as slightly
    // harder than it should.
    //
    // Exact because clamp has an antiderivative in closed form -
    // clamp(x,0,1)^2/2 + max(x-1,0) - so the average over a linear ramp is the
    // difference of that at the ends over the run. A vertical segment has no
    // run, so its value is taken directly rather than through a division by
    // zero; both are evaluated and one selected, since the two arms cost less
    // than the branch would.
    static GPU::Float clampedIntegral(const GPU::Float& x)
    {
        auto inside = clamp(x, 0.f, 1.f);
        return inside * inside * 0.5f + max(x - 1.f, 0.f);
    }

    static GPU::Float meanClampedX(const GPU::Float& from, const GPU::Float& to)
    {
        auto run = to - from;
        auto flat = abs(run) < 1e-6f;

        auto ramp =
            (clampedIntegral(to) - clampedIntegral(from)) / select(flat, 1.f, run);

        return select(flat, clamp(from, 0.f, 1.f), ramp);
    }

    // Segments grouped by the tile that walks them, four floats each
    // (x0, y0, x1, y1), already in the coverage texture's own pixel space. A
    // segment crossing a tile boundary appears once under each tile it crosses.
    // Every path in the batch, end to end; a record says where each one starts.
    GPU::Uniform<GPU::InputBuffer> segments;

    // Where each tile's run of segments starts, one entry per tile and a last
    // one holding the total, so a tile's run is [tileOffsets[t], tileOffsets[t+1]).
    // Counts, carried as floats because a storage buffer is a run of floats -
    // exact well past any segment count this rasterizer is for. Each path's
    // offsets are relative to its own segment run.
    GPU::Uniform<GPU::InputBuffer> tileOffsets;

    // The winding entering a tile from the left, as the steps it changes at:
    // (tile column, winding) pairs, ordered by column, grouped by pixel row.
    //
    // Per pixel row rather than per tile because a segment ending inside a
    // tile's band covers some of its rows and not others, so one number per tile
    // would be the winding at only one of them. As steps rather than as a value
    // per row per column because the second is the area's size and this is the
    // outline's: a window-sized ellipse changes winding five times across two
    // hundred columns, and storing the two hundred cost more CPU than the rest
    // of the rasterization together.
    GPU::Uniform<GPU::InputBuffer> backdropSteps;

    // Where each pixel row's run of steps starts, with a last entry holding the
    // total - so a row's steps are [backdropRows[y], backdropRows[y + 1]), and a
    // row no edge crosses is an empty range rather than a run of zeroes.
    GPU::Uniform<GPU::InputBuffer> backdropRows;

    // The same backdrop written out in full instead - the winding at every tile
    // column of every pixel row. Priced by the area rather than by the outline,
    // which is dearer to build and cheaper to read, and so is what a path whose
    // outline crosses nearly every row uses instead. A path fills one of the
    // two; the other holds nothing for it.
    //
    // Nothing on the CPU fills it or ever sees it: three kernels ahead of this
    // one clear it, scatter the outline's crossings into it and sum each row -
    // see BackdropKernels.h - so an array priced by the area costs the area
    // nowhere but on the GPU, which is the only place it was ever cheap.
    GPU::Uniform<GPU::AtomicBuffer> cells;

    GPU::Uniform<GPU::WritableTexture2D> coverage;

    // How many blocks across the dispatch grid is, which is what turns a group
    // position into a block index.
    GPU::Uniform<GPU::UInt> gridColumns;

    EACP_SHADER(segments,
                tileOffsets,
                backdropSteps,
                backdropRows,
                cells,
                records,
                pathStarts,
                coverage,
                gridColumns,
                pathCount)
};
} // namespace eacp::GPUWidgets
