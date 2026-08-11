#include "PathRasterizer.h"

#include <cmath>

namespace eacp::GPUWidgets
{
namespace
{
constexpr auto tileSize = CoverageKernel::tileSize;

// [tileOf(from), tileAfter(to) - 1] is every tile a span touches.
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

    // Holding the owned texture would keep a path's worth of pixels alive.
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
    segmentTests = -1;
    soloStale = true;

    if (path.isEmpty() || scale <= 0.f)
        return;

    // A pixel of margin on every side, for coverage spilling past the bounds.
    auto bounds = path.getBounds();
    auto left = std::floor(bounds.x * scale) - 1.f;
    auto top = std::floor(bounds.y * scale) - 1.f;
    auto right = std::ceil((bounds.x + bounds.w) * scale) + 1.f;
    auto bottom = std::ceil((bounds.y + bounds.h) * scale) + 1.f;

    coverageWidth = (int) (right - left);
    coverageHeight = (int) (bottom - top);
    covered = {
        left / scale, top / scale, (right - left) / scale, (bottom - top) / scale};

    tilesWide = (coverageWidth + tileSize - 1) / tileSize;
    tilesHigh = (coverageHeight + tileSize - 1) / tileSize;
    entryBound = 0;

    for (const auto& sub: path.getSubPaths())
    {
        const auto& points = sub.points;

        if (points.size() < 2)
            continue;

        // Wrapping emits the closing segment, so every sub-path reaches the
        // kernel as a closed loop whether or not close() was called.
        for (auto i = 0; i < points.size(); ++i)
        {
            const auto& from = points[i];
            const auto& to = points[(i + 1) % points.size()];

            auto fromY = from.y * scale - top;
            auto toY = to.y * scale - top;

            // A horizontal segment covers nothing; dropped once, not per pixel.
            if (fromY == toY)
                continue;

            auto fromX = from.x * scale - left;
            auto toX = to.x * scale - left;

            segments.add(fromX);
            segments.add(fromY);
            segments.add(toX);
            segments.add(toY);

            entryBound += boundEntriesOf(fromX, fromY, toX, toY);
        }
    }

    if (isEmpty())
        return;

    evenOdd = rule == FillRule::EvenOdd ? 1 : 0;
}

// A monotone segment costs one tile per row it crosses plus one per column
// boundary, which telescopes into its width in tiles. Overshoots the true count
// by about a third, in constant time rather than by clipping.
int PathRasterizer::boundEntriesOf(float fromX,
                                   float fromY,
                                   float toX,
                                   float toY) const
{
    auto firstRow = std::max(tileOf(std::min(fromY, toY)), 0);
    auto lastRow = std::min(tileAfter(std::max(fromY, toY)) - 1, tilesHigh - 1);
    auto rows = lastRow - firstRow + 1;

    if (rows <= 0)
        return 0;

    auto width = std::abs(toX - fromX) / (float) tileSize;

    return std::min(rows * tilesWide, rows + (int) std::ceil(width));
}

int PathRasterizer::getCellCount() const
{
    return tilesWide * coverageHeight;
}

long long PathRasterizer::getSegmentTests() const
{
    countTiles();
    return segmentTests;
}

int PathRasterizer::getEntryCount() const
{
    countTiles();
    return entryCount;
}

// The binning kernel's clip, repeated on the CPU purely for its counts, so a
// bench need not read the tile offsets back off the GPU.
void PathRasterizer::countTiles() const
{
    if (segmentTests >= 0)
        return;

    segmentTests = 0;
    entryCount = 0;

    auto count = segments.size() / 4;

    for (auto index = 0; index < count; ++index)
    {
        const auto* segment = segments.data() + index * 4;

        auto topY = std::min(segment[1], segment[3]);
        auto bottomY = std::max(segment[1], segment[3]);
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
            auto firstColumn = std::max(tileOf(std::min(enters, leaves)), 0);
            auto lastColumn = std::min(beyond - 1, tilesWide - 1);

            // The last tile of a row or column is a partial one.
            auto rows = std::min(tileSize, coverageHeight - row * tileSize);

            for (auto column = firstColumn; column <= lastColumn; ++column)
            {
                segmentTests += (long long) rows
                                * (long long) std::min(
                                    tileSize, coverageWidth - column * tileSize);
                ++entryCount;
            }
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

    // Re-dispatching an unchanged rasterizer uploads nothing.
    if (soloStale)
    {
        solo->begin(*getTargetTexture());
        solo->add(*this);
        soloStale = false;
    }

    solo->dispatch(pass);
}
} // namespace eacp::GPUWidgets
