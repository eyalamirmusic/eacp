#pragma once

#include "../Common.h"

namespace eacp::GPUWidgets
{
// Neither shader language has an atomic float add, so the backdrop winding is
// summed as fixed point: 20 fractional bits, a whole part reaching +-2048.
constexpr float backdropFixedScale = 1048576.f;

// One shared instance per kernel type. Safe because every uniform is set
// immediately before its dispatch and encoding is single-threaded; built on
// first use because it needs the Device, and so tears down before it.
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

// A stage whose work items belong to many paths, and the search that says which.
struct PathIndexedKernel : GPU::ComputeProgram
{
    // Tile side in coverage pixels. Four of the 8x8 threadgroups the 2D dispatch
    // uses, so every thread of a group reads the same tile offsets.
    static constexpr int tileSize = 16;

    // Two float4 reads; the tile and block grid widths are derived, not stored.
    static constexpr int recordFloats = 8;

    // The last path whose run starts at or before the item. Paths with an empty
    // run share their successor's start and are stepped over.
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

        // lo is the first run starting past the item; clamped rather than
        // decremented, since entry zero is never past anything.
        return toUInt(max(lo.get(), 1) - 1);
    }

    // Split so the four stages needing only the shape skip the second read.
    GPU::Float4 recordShape(const GPU::UInt& path)
    {
        return records.read4(recordAt(path));
    }

    GPU::Float4 recordPlace(const GPU::UInt& path)
    {
        return records.read4(recordAt(path) + 1u);
    }

    static GPU::UInt tilesWideOf(const GPU::UInt& width)
    {
        return (width + (unsigned) (tileSize - 1)) / (unsigned) tileSize;
    }

    // Where each path's run of this stage's items starts, plus a terminating
    // total. Its own buffer so the search's keys sit next to each other.
    GPU::Uniform<GPU::InputBuffer> pathStarts;

    // Per path: cell base, width, height, tile base, then fill rule and target
    // origin. The eighth float is spare, and deliberately last.
    GPU::Uniform<GPU::InputBuffer> records;

    // Paths, not runs: pathStarts' last entry belongs to no path.
    GPU::Uniform<GPU::Int> pathCount;

private:
    static GPU::UInt recordAt(const GPU::UInt& path)
    {
        return path * (unsigned) (recordFloats / 4);
    }
};

// One thread per pixel, working in a unit box centred on it: each segment adds
// the signed area to its right, summing to exact coverage. A thread walks only
// its own tile, everything left of it arriving as the backdrop winding.
struct CoverageKernel final : PathIndexedKernel
{
    // Paths are laid out in the grid a block at a time, so a group's 64 threads
    // are the 8x8 corner of exactly one path.
    static constexpr int blockSize = GPU::ComputeProgram::groupSize2D;

    CoverageKernel() { compile(); }

    void define() override
    {
        // A block is a threadgroup, so the block index comes from the group id:
        // everything up to the loop is then provably uniform and computed once
        // per group rather than once per thread.
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

        // A block past the end of the batch lands below the last path, which the
        // guard below retires - so the search needs none of its own.
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

                   // A mask in all four channels: R8Unorm is not guaranteed for
                   // a typed UAV store, so the texture is RGBA8. Segments are in
                   // the path's own space, so only the origin places them.
                   write(coverage,
                         pixelX + originX,
                         pixelY + originY,
                         float4(value, value, value, value));
               });
    }

    // In the path's own space; the bases carry it into the batch's buffers.
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

        // Everything left of the tile spans the pixel's whole row-slice, so it
        // arrives as one already-summed number rather than a list to walk.
        auto winding = var(cellAt(cellBase + column * height + pixelY));

        // Batch-wide offsets, needing no per-path base, and held in locals since
        // the generated while header re-tests the condition.
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

                 // The vertical span inside this pixel: zero above, below or
                 // horizontal, which is also the only slope that divides by zero.
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

        // Even-odd folds the winding into a triangle wave, non-zero saturates.
        // Both are computed and selected: a branch would only cost.
        auto folded = fract(total * 0.5f) * 2.f;
        auto evenOddCoverage = min(folded, 2.f - folded);
        auto nonZeroCoverage = min(total, 1.f);

        return select(evenOdd != 0.f, evenOddCoverage, nonZeroCoverage);
    }

    // The winding entering this tile column at this pixel row, converted back
    // out of backdropFixedScale. Column-major, the order the scan writes in.
    GPU::Float cellAt(const GPU::UInt& index)
    {
        return toFloat(toInt(cells.load(index))) * (1.f / backdropFixedScale);
    }

    // The antiderivative of clamp(x, 0, 1), which makes meanClampedX exact
    // rather than a midpoint sample.
    static GPU::Float clampedIntegral(const GPU::Float& x)
    {
        auto inside = clamp(x, 0.f, 1.f);
        return inside * inside * 0.5f + max(x - 1.f, 0.f);
    }

    // The fraction of the pixel row-slice left of the segment. A vertical
    // segment has no run, so both arms are evaluated and one selected.
    static GPU::Float meanClampedX(const GPU::Float& from, const GPU::Float& to)
    {
        auto run = to - from;
        auto flat = abs(run) < 1e-6f;

        auto ramp =
            (clampedIntegral(to) - clampedIntegral(from)) / select(flat, 1.f, run);

        return select(flat, clamp(from, 0.f, 1.f), ramp);
    }

    // Segments grouped by tile, four floats each (x0, y0, x1, y1) in coverage
    // pixel space, repeated under every tile they cross. Filled by BinKernels.h.
    GPU::Uniform<GPU::InputBuffer> tileSegments;

    // A tile's run is [tileOffsets[t], tileOffsets[t + 1]), one entry per tile of
    // the batch plus a total. Integers, being built by the prefix sum.
    GPU::Uniform<GPU::AtomicBuffer> tileOffsets;

    // The winding entering a tile from the left, per pixel row rather than per
    // tile since a segment may end inside the tile's band. Filled by
    // BackdropKernels.h.
    GPU::Uniform<GPU::AtomicBuffer> cells;

    GPU::Uniform<GPU::WritableTexture2D> coverage;

    // Turns a group position into a block index.
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
