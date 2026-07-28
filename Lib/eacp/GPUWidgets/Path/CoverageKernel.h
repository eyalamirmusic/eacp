#pragma once

#include "../Common.h"

namespace eacp::GPUWidgets
{
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
// it as a single number - the winding entering the tile from the left, summed on
// the CPU for the thread's own pixel row - so a pixel pays for the outline
// passing near it and not for the outline existing. See PathRasterizer, which
// does the binning, the transform into pixel space, and the backdrop sum.
//
// Horizontal segments contribute nothing, and are dropped on the CPU rather than
// guarded against here.
struct CoverageKernel final : GPU::ComputeProgram
{
    // Coverage pixels along one side of a tile. A thread reads two offsets and a
    // backdrop before its loop, so tiles want to be big enough that the three
    // reads disappear against the segments they save; they also want to be small
    // enough that a tile holds only the outline near it. Sixteen is four of the
    // 8x8 threadgroups the 2D dispatch uses, so the offsets a group reads are
    // the same for every thread in it.
    static constexpr int tileSize = 16;

    CoverageKernel() { compile(); }

    void define() override
    {
        auto pixel = threadPosition();
        auto pixelX = toFloat(pixel.x);
        auto pixelY = toFloat(pixel.y);

        auto column = pixel.x / (unsigned) tileSize;
        auto tile = (pixel.y / (unsigned) tileSize) * tilesWide + column;

        // Everything left of this tile covers the pixel's whole row-slice, so
        // its contribution depends on the row and not on the column - which is
        // what lets the CPU sum it once per row into a number the thread starts
        // from instead of a list it walks.
        auto winding = var(backdrops[pixel.y * tilesWide + column]);

        // Held in locals rather than re-read: the loop condition is re-tested in
        // the generated while header, and these do not change under it.
        auto index = var(toInt(tileOffsets[tile]));
        auto last = var(toInt(tileOffsets[tile + 1u]));

        loop(index < last,
             [&]
             {
                 auto segment = segments.read4(toUInt(index));

                 auto ax = segment.x() - pixelX;
                 auto ay = segment.y() - pixelY;
                 auto bx = segment.z() - pixelX;
                 auto by = segment.w() - pixelY;

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
        // on: the choice is uniform across the whole dispatch, so a branch here
        // would buy nothing and cost the divergence check.
        auto folded = fract(total * 0.5f) * 2.f;
        auto evenOddCoverage = min(folded, 2.f - folded);
        auto nonZeroCoverage = min(total, 1.f);

        auto value = select(evenOdd != 0, evenOddCoverage, nonZeroCoverage);

        // The same coverage in all four channels. A one-channel mask is what
        // this is, but R8Unorm is outside the set a typed UAV store is
        // guaranteed for - see supportsComputeWrite - so the texture is RGBA8
        // and whoever samples it reads whichever channel it likes.
        //
        // The origin is what lets several paths share one texture: the grid is
        // dispatched over this path's own size and the segments arrive in its
        // own space, so only the write moves. Two adds per thread, against
        // giving every path a texture and every draw a bind of its own.
        write(coverage,
              pixel.x + originX,
              pixel.y + originY,
              float4(value, value, value, value));
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
    GPU::Uniform<GPU::InputBuffer> segments;

    // Where each tile's run of segments starts, one entry per tile and a last
    // one holding the total, so a tile's run is [tileOffsets[t], tileOffsets[t+1]).
    // Counts, carried as floats because a storage buffer is a run of floats -
    // exact well past any segment count this rasterizer is for.
    GPU::Uniform<GPU::InputBuffer> tileOffsets;

    // The winding entering a tile from the left, one entry per pixel row per
    // tile column, indexed row-major. Per row rather than per tile because a
    // segment ending inside the tile's band covers some of its rows and not
    // others - a single number per tile would be the winding at only one of them.
    GPU::Uniform<GPU::InputBuffer> backdrops;

    GPU::Uniform<GPU::WritableTexture2D> coverage;
    GPU::Uniform<GPU::UInt> tilesWide;
    GPU::Uniform<GPU::Int> evenOdd; // 0 = non-zero fill rule, 1 = even-odd
    GPU::Uniform<GPU::UInt> originX; // where this path sits in the target
    GPU::Uniform<GPU::UInt> originY;

    EACP_SHADER(segments,
                tileOffsets,
                backdrops,
                coverage,
                tilesWide,
                evenOdd,
                originX,
                originY)
};
} // namespace eacp::GPUWidgets
