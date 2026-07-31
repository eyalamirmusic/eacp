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

// One kernel of each sort for every batch there will ever be. It is the same
// program in all of them, and building one costs a shader library and a compute
// pipeline.
//
// Shared state is safe here because a dispatch sets every uniform it reads
// immediately before issuing it, and command encoding is single-threaded. Built
// on first use rather than at load, since it needs the Device - which also puts
// its destruction before the Device's own, statics tearing down in reverse.
template <typename Kernel>
Kernel& sharedKernel()
{
    struct Prepared
    {
        Prepared() { kernel.prepare(); }

        Kernel kernel;
    };

    static auto prepared = Prepared {};
    return prepared.kernel;
}

// A dispatch whose work belongs to many paths, and the search that says which.
// Every stage of a batched rasterization needs it - a thread has an item index
// and nothing else - and they need it identically, so it is written once here.
struct PathIndexedKernel : GPU::ComputeProgram
{
    // Coverage pixels along one side of a tile. A thread reads two offsets and a
    // backdrop before its loop, so tiles want to be big enough that the three
    // reads disappear against the segments they save; they also want to be small
    // enough that a tile holds only the outline near it. Sixteen is four of the
    // 8x8 threadgroups the 2D dispatch uses, so the offsets a group reads are
    // the same for every thread in it.
    static constexpr int tileSize = 16;

    // Floats per path record: two float4 reads, and every field of them used.
    // How wide the path's tile grid and its block grid are would be a ninth and
    // a tenth, and are derived from the width instead - a shift and an add
    // against a third read and four more floats per path.
    static constexpr int recordFloats = 8;

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

    // The two reads a record is, named rather than counted. A stage that took
    // the wrong one would read plausible numbers out of the neighbouring field,
    // so the offsets are written once here, beside the constant, rather than
    // spelled at each call.
    //
    // Where the split falls is not arbitrary. Four of the six stages want only
    // where the path's cells and tiles begin and how big its coverage is, so
    // those four numbers share a read and none of them touches the second: the
    // binner reads it once per segment, and a segment is the most numerous thing
    // in the batch.
    GPU::Float4 recordShape(const GPU::UInt& path)
    {
        return records.read4(recordAt(path));
    }

    GPU::Float4 recordPlace(const GPU::UInt& path)
    {
        return records.read4(recordAt(path) + 1u);
    }

    // How many tile columns a path of this width has, which is the one number
    // the record leaves out because it is a shift away from one it holds.
    static GPU::UInt tilesWideOf(const GPU::UInt& width)
    {
        return (width + (unsigned) (tileSize - 1)) / (unsigned) tileSize;
    }

    // Where each path's run of this stage's items starts, with a last entry
    // holding the total. The one buffer a thread reads before it knows anything
    // at all, which is why it is its own buffer rather than a field of the
    // records: the search wants its keys next to each other.
    GPU::Uniform<GPU::InputBuffer> pathStarts;

    // Eight floats per path: where its backdrop cells and its tiles begin and
    // how big its coverage is, then which fill rule it takes and where it sits
    // in the target. Read by every stage, through the two accessors above.
    //
    // The last is spare, and deliberately last: three reads of four is the
    // shape, and padding in the middle of a record is exactly what once put the
    // fill rule in the wrong one.
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
// not for the outline existing. See BinKernels.h, which sorts the segments into
// tiles, and BackdropKernels.h for where that number comes from. The CPU's part
// is the transform into pixel space and nothing else.
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

        auto shape = recordShape(path);
        auto place = recordPlace(path);

        auto cellBase = toUInt(shape.x());
        auto width = toUInt(shape.y());
        auto height = toUInt(shape.z());
        auto tileBase = toUInt(shape.w());

        auto originX = toUInt(place.y());
        auto originY = toUInt(place.z());

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
                                           tileBase,
                                           cellBase,
                                           height,
                                           tilesWideOf(width),
                                           place.x());

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

        // Everything left of this tile covers the pixel's whole row-slice, so
        // its contribution depends on the row and not on the column - which is
        // what lets it arrive as a number the thread starts from instead of a
        // list it walks. One load, the stages ahead of this one having already
        // summed it along the row.
        auto winding = var(cellAt(cellBase + column * height + pixelY));

        // Held in locals rather than re-read: the loop condition is re-tested in
        // the generated while header, and these do not change under it.
        //
        // The offsets are the batch's own, not the path's: the prefix sum that
        // produced them ran over every tile of every path at once, so a tile's
        // run is already where it is in the one segment array and nothing has a
        // base to add.
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

                 index += 1u;
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

    // The winding entering this tile column at this pixel row, which the stages
    // ahead of this one left summed in place: the lookup is the read and nothing
    // else.
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
    // Every path in the batch, end to end.
    //
    // Nothing on the CPU fills this: the binning kernel clips each segment to
    // the tile rows it crosses and a counting sort puts it here - see
    // BinKernels.h.
    GPU::Uniform<GPU::InputBuffer> tileSegments;

    // Where each tile's run of segments starts, one entry per tile of every path
    // in the batch and a last one holding the total, so a tile's run is
    // [tileOffsets[t], tileOffsets[t + 1]). Built by the prefix sum over what
    // the binner counted, which is why it is read as integers rather than as the
    // floats every uploaded buffer holds.
    GPU::Uniform<GPU::AtomicBuffer> tileOffsets;

    // The winding entering a tile from the left: one value at every tile column
    // of every pixel row.
    //
    // Per pixel row rather than per tile because a segment ending inside a
    // tile's band covers some of its rows and not others, so one number per tile
    // would be the winding at only one of them.
    //
    // Nothing on the CPU fills it or ever sees it: three kernels ahead of this
    // one clear it, scatter the outline's crossings into it and sum each row -
    // see BackdropKernels.h - so a backdrop priced by the area costs the area
    // nowhere but on the GPU, which is the only place it was ever cheap.
    GPU::Uniform<GPU::AtomicBuffer> cells;

    GPU::Uniform<GPU::WritableTexture2D> coverage;

    // How many blocks across the dispatch grid is, which is what turns a group
    // position into a block index.
    GPU::Uniform<GPU::UInt> gridColumns;

    EACP_SHADER(tileSegments,
                tileOffsets,
                cells,
                records,
                pathStarts,
                coverage,
                gridColumns,
                pathCount)
};
} // namespace eacp::GPUWidgets
