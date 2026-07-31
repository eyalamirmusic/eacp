#include "PathRasterizer.h"

#include <cmath>
#include <cstring>

namespace eacp::GPUWidgets
{
namespace
{
constexpr auto tileSize = CoverageKernel::tileSize;

// The tile a coordinate falls in, and the first tile entirely past it. Together
// they bracket a span: [tileOf(from), tileAfter(to) - 1] is every tile the span
// touches, and tileAfter(to) is the first one it does not reach.
int tileOf(float coordinate)
{
    return (int) std::floor(coordinate / (float) tileSize);
}

int tileAfter(float coordinate)
{
    return (int) std::ceil(coordinate / (float) tileSize);
}

GPU::TextureDescriptor describeCoverage(int width, int height)
{
    auto descriptor = GPU::TextureDescriptor {};
    descriptor.width = width;
    descriptor.height = height;
    descriptor.format = GPU::TextureFormat::RGBA8Unorm;
    descriptor.computeWrite = true;
    return descriptor;
}

} // namespace

void PathRasterizer::setScale(float pixelsPerUnit)
{
    scale = pixelsPerUnit;
}

bool PathRasterizer::isEmpty() const
{
    return segments.empty() || coverageWidth <= 0 || coverageHeight <= 0;
}

void PathRasterizer::setTarget(const GPU::Texture& texture,
                               int originXToUse,
                               int originYToUse)
{
    target = &texture;
    originX = originXToUse;
    originY = originYToUse;
    soloStale = true;

    // Its own texture is now dead weight, and holding it would keep a whole
    // path's worth of pixels alive for as long as the shape exists.
    coverageTexture.reset();
}

void PathRasterizer::clearTarget()
{
    target = nullptr;
    originX = 0;
    originY = 0;
    soloStale = true;
}

const GPU::Texture* PathRasterizer::getTargetTexture() const
{
    if (target != nullptr)
        return target;

    return coverageTexture.has_value() ? &*coverageTexture : nullptr;
}

void PathRasterizer::setPath(const Path& path, FillRule rule)
{
    segments.clear();
    covered = {};
    coverageWidth = 0;
    coverageHeight = 0;
    segmentTests = 0;
    soloStale = true;

    if (path.isEmpty() || scale <= 0.f)
        return;

    // A pixel of margin on every side: an edge landing exactly on the bounding
    // box still spills coverage into the pixel beyond it.
    auto bounds = path.getBounds();
    auto left = std::floor(bounds.x * scale) - 1.f;
    auto top = std::floor(bounds.y * scale) - 1.f;
    auto right = std::ceil((bounds.x + bounds.w) * scale) + 1.f;
    auto bottom = std::ceil((bounds.y + bounds.h) * scale) + 1.f;

    coverageWidth = (int) (right - left);
    coverageHeight = (int) (bottom - top);
    covered = {
        left / scale, top / scale, (right - left) / scale, (bottom - top) / scale};

    for (const auto& sub: path.getSubPaths())
    {
        const auto& points = sub.points;

        if (points.size() < 2)
            continue;

        // Wrapping past the last point emits the closing segment explicitly, so
        // every sub-path reaches the kernel as a closed loop - which a fill
        // treats it as whether or not close() was called.
        for (auto i = 0; i < points.size(); ++i)
        {
            const auto& from = points[i];
            const auto& to = points[(i + 1) % points.size()];

            auto fromY = from.y * scale - top;
            auto toY = to.y * scale - top;

            // A horizontal segment contributes nothing to any pixel, so it is
            // dropped once here rather than guarded against once per pixel.
            if (fromY == toY)
                continue;

            segments.add(from.x * scale - left);
            segments.add(fromY);
            segments.add(to.x * scale - left);
            segments.add(toY);
        }
    }

    if (isEmpty())
        return;

    evenOdd = rule == FillRule::EvenOdd ? 1 : 0;

    buildTiles();
}

// One crossing of the outline into one tile column, over the part of one tile
// row's band it spans. It is what the backdrop's array is built from, and there
// is one per segment per band it crosses - so recording the crossing rather than
// what it expands to is what keeps this side of the work priced by the outline.
//
// Directed the way the segment runs, so the sign of the span is the sign of the
// winding and there is no third number to carry. Nothing else is done with it
// here: the array these belong to is built on the GPU, and the whole point is
// that the CPU never walks its area.
void PathRasterizer::addCrossing(float direction, float fromY, float toY, int column)
{
    if (column >= tilesWide)
        return;

    crossings.add((float) column);
    crossings.add(direction > 0.f ? fromY : toY);
    crossings.add(direction > 0.f ? toY : fromY);
}

// Splits every segment across the tile rows it crosses, and within each row
// files it under the tiles it actually reaches. Clipping to the row first is
// what keeps a long diagonal honest: binned by its bounding box it would land in
// every tile of a square, when it passes through a couple per row.
//
// Everything to the left of the tiles it reaches is not filed anywhere. It
// covers those pixels' whole row-slices, so its contribution turns on the pixel
// row alone, and it goes to the backdrop instead - a number a thread starts its
// winding from rather than a list it walks.
void PathRasterizer::buildTiles()
{
    tilesWide = (coverageWidth + tileSize - 1) / tileSize;
    tilesHigh = (coverageHeight + tileSize - 1) / tileSize;

    auto tileCount = tilesWide * tilesHigh;

    runs.clear();
    tileOffsets.assign(tileCount + 1, 0.f);
    crossings.clear();

    auto count = segments.size() / 4;

    for (auto index = 0; index < count; ++index)
    {
        const auto* segment = segments.data() + index * 4;

        auto topY = std::min(segment[1], segment[3]);
        auto bottomY = std::max(segment[1], segment[3]);
        auto direction = segment[3] > segment[1] ? 1.f : -1.f;
        auto slope = (segment[2] - segment[0]) / (segment[3] - segment[1]);

        auto firstRow = std::max(tileOf(topY), 0);
        auto lastRow = std::min(tileAfter(bottomY) - 1, tilesHigh - 1);

        for (auto row = firstRow; row <= lastRow; ++row)
        {
            auto bandTop = std::max(topY, (float) (row * tileSize));
            auto bandBottom = std::min(bottomY, (float) ((row + 1) * tileSize));

            if (bandBottom <= bandTop)
                continue;

            auto enters = segment[0] + (bandTop - segment[1]) * slope;
            auto leaves = segment[0] + (bandBottom - segment[1]) * slope;

            auto beyond = std::max(tileAfter(std::max(enters, leaves)), 0);
            addCrossing(direction, bandTop, bandBottom, beyond);

            auto firstColumn = std::max(tileOf(std::min(enters, leaves)), 0);
            auto lastColumn = std::min(beyond - 1, tilesWide - 1);

            if (lastColumn < firstColumn)
                continue;

            runs.add(TileRun {
                index, row * tilesWide + firstColumn, lastColumn - firstColumn + 1});

            for (auto column = firstColumn; column <= lastColumn; ++column)
                tileOffsets[row * tilesWide + column + 1] += 1.f;
        }
    }

    // Counted one tile late above, so summing turns the counts into the start of
    // each tile's run and leaves the total in the last entry.
    for (auto tile = 1; tile <= tileCount; ++tile)
        tileOffsets[tile] += tileOffsets[tile - 1];

    tileCursor.resize(tileCount);

    for (auto tile = 0; tile < tileCount; ++tile)
        tileCursor[tile] = (int) tileOffsets[tile];

    tileSegments.resize((int) tileOffsets[tileCount] * 4);

    for (const auto& run: runs)
    {
        const auto* source = segments.data() + run.segment * 4;

        for (auto tile = 0; tile < run.tiles; ++tile)
        {
            auto at = tileCursor[run.firstTile + tile]++;
            std::memcpy(tileSegments.data() + at * 4, source, sizeof(float) * 4);
        }
    }

    // A path whose segments all fall outside the coverage rect leaves nothing to
    // bind, and a zero-length buffer is not a buffer. One record of zeroes is a
    // horizontal segment, which no tile's run reaches and which would contribute
    // nothing if one did.
    if (tileSegments.empty())
        tileSegments.resize(4);

    countSegmentTests();
}

int PathRasterizer::getCellCount() const
{
    return tilesWide * coverageHeight;
}

void PathRasterizer::countSegmentTests()
{
    segmentTests = 0;

    for (auto row = 0; row < tilesHigh; ++row)
    {
        // The last tile of a row or column is a partial one, and counting it
        // whole would flatter the number this exists to report.
        auto rows = std::min(tileSize, coverageHeight - row * tileSize);

        for (auto column = 0; column < tilesWide; ++column)
        {
            auto tile = row * tilesWide + column;
            auto listed = tileOffsets[tile + 1] - tileOffsets[tile];
            auto pixels =
                rows * std::min(tileSize, coverageWidth - column * tileSize);

            segmentTests += (long long) pixels * (long long) listed;
        }
    }
}

void PathRasterizer::ensureOwnTexture()
{
    if (coverageTexture.has_value() && coverageTexture->width() == coverageWidth
        && coverageTexture->height() == coverageHeight)
        return;

    coverageTexture.emplace(GPU::Device::shared(),
                            describeCoverage(coverageWidth, coverageHeight),
                            nullptr);
}

void PathRasterizer::dispatch(GPU::ComputePass& pass)
{
    if (isEmpty())
        return;

    if (target == nullptr)
        ensureOwnTexture();

    if (!solo.has_value())
        solo.emplace();

    // Gathered again only when there is something new to gather. Dispatching the
    // same rasterizer repeatedly - a demo redrawing a static path every frame -
    // then costs a dispatch and no bytes at all, which is what it cost when
    // every rasterizer owned its buffers.
    if (soloStale)
    {
        solo->begin(*getTargetTexture());
        solo->add(*this);
        soloStale = false;
    }

    solo->dispatch(pass);
}
} // namespace eacp::GPUWidgets
