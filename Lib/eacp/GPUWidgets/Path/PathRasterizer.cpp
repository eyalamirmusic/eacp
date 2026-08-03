#include "PathRasterizer.h"

#include <cmath>

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
    segmentTests = -1;
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

    tilesWide = (coverageWidth + tileSize - 1) / tileSize;
    tilesHigh = (coverageHeight + tileSize - 1) / tileSize;
    entryBound = 0;

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

// How much room one segment's binning needs, which is the only thing about
// binning that has to be settled before the dispatch.
//
// The exact count is what the clip finds, and the clip is on the GPU - so this
// bounds it instead, in constant time and in the loop that emits the segment,
// which is what keeps it off a second pass over an array that is megabytes on a
// dense path.
//
// A segment is straight, so its x runs one way as its y does and the bands it
// crosses hand their x-ranges to each other end to end. A band therefore costs
// one column for itself and one for each column boundary the segment crosses
// inside it - and summed over the bands, the second term telescopes into the
// segment's own width in tiles. So the rows it crosses plus its width in tiles
// is an upper bound; the rows times the whole grid's width is the other one,
// which is smaller when the segment is nearly horizontal.
//
// It comes out about a third over the true count, which buys an array a third
// too big and no work at all - against a clip walked here to learn the exact
// number, which is the whole of what this rung moved.
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

// The same clip the binning kernel does, walked here for its counts alone: what
// the dispatch is about to cost, and how many entries it will really produce
// against the bound the array was sized to.
//
// This is the one thing left on this side that is priced by the outline times
// the tiles it crosses, and it is deliberate: nothing in an interface reads
// either number, and a bench or a test that wants them would otherwise have to
// read the tile offsets back off the GPU.
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

            // The last tile of a row or column is a partial one, and counting it
            // whole would flatter the number this exists to report.
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
