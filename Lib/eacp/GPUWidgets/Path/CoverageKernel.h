#pragma once

#include "../Common.h"

namespace eacp::GPUWidgets
{
// The EDSL-authored kernel behind PathRasterizer: one thread per pixel, walking
// every segment of a path and accumulating that segment's signed contribution to
// its own pixel's coverage.
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
// Horizontal segments contribute nothing, and are dropped on the CPU rather than
// guarded against here.
//
// Segments arrive already in the coverage texture's own pixel space, four floats
// per segment (x0, y0, x1, y1) - see PathRasterizer, which does the transform
// once at upload instead of per pixel per segment.
struct CoverageKernel final : GPU::ComputeProgram
{
    CoverageKernel() { compile(); }

    void define() override
    {
        auto pixel = threadPosition();
        auto pixelX = toFloat(pixel.x);
        auto pixelY = toFloat(pixel.y);

        auto winding = var(0.f);
        auto index = var(0);

        loop(index < segmentCount,
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

    GPU::Uniform<GPU::InputBuffer> segments; // 4 floats per directed segment
    GPU::Uniform<GPU::WritableTexture2D> coverage;
    GPU::Uniform<GPU::Int> segmentCount;
    GPU::Uniform<GPU::Int> evenOdd; // 0 = non-zero fill rule, 1 = even-odd
    GPU::Uniform<GPU::UInt> originX; // where this path sits in the target
    GPU::Uniform<GPU::UInt> originY;

    EACP_SHADER(segments, coverage, segmentCount, evenOdd, originX, originY)
};
} // namespace eacp::GPUWidgets
