#include "PathRasterizer.h"

#include <cmath>
#include <cstring>

namespace eacp::GPUWidgets
{
namespace
{
constexpr auto tileSize = CoverageKernel::tileSize;

// How much emptier than its crossings the array form has to be before the step
// form is worth it. Both were measured across the whole PathBench corpus at
// several factors: the array wins where a row's crossings are dense enough that
// the steps barely compress it and the kernel still pays to search them, and
// the steps win by five times where they do not. The crossover is broad and
// nothing in the corpus lands near it, so this is a measured constant rather
// than a tuned one.
//
// Measured against an array the CPU built, though. It is built on the GPU now,
// so what it costs is O(area) of GPU memory and two stages ahead of the coverage
// kernel, against a per-pixel search - a different trade, whose answer has not
// been re-derived.
constexpr auto sparseBackdropMargin = 8;

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
    backdropSpan = 0.f;
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

            // Accumulated here because this is where the segment is already in
            // hand, and it is what chooseBackdropForm spends - one subtraction
            // rather than a second walk over all of them.
            backdropSpan += std::abs(toY - fromY);
        }
    }

    if (isEmpty())
        return;

    evenOdd = rule == FillRule::EvenOdd ? 1 : 0;

    buildTiles();
}

void PathRasterizer::addBackdrop(
    float direction, float fromY, float toY, int column, int band)
{
    if (column >= tilesWide)
        return;

    if (sparseBackdrop)
    {
        bandRuns.add(BandRun {band, column, fromY, toY, direction});
        return;
    }

    // Directed the way the segment runs, so the sign of the span is the sign of
    // the winding and there is no third number to carry. Nothing else is done
    // with it here: the array these belong to is built on the GPU, and the whole
    // point of the form is that the CPU never walks its area.
    crossings.add((float) column);
    crossings.add(direction > 0.f ? fromY : toY);
    crossings.add(direction > 0.f ? toY : fromY);
}

// Which of the two backdrop forms this path is shaped for, settled before the
// binning loop, which is what lets a crossing be written straight into the shape
// the form it belongs to wants rather than into one both could be built from.
//
// It turns on how empty the array would be, against the pixel rows the crossings
// between them cover. Those are bounded from the outline's total vertical travel
// rather than counted: a segment's span rounds out to at most two extra rows,
// and splitting it across bands repeats at most one row per boundary it crosses.
// Both are loose upward, which only ever chooses the array, and the corpus this
// was measured on has nothing within an order of magnitude of the crossover on
// either side.
void PathRasterizer::chooseBackdropForm()
{
    constexpr auto bandRepeat = 1.f + 1.f / (float) tileSize;
    constexpr auto roundingPerSegment = 4;

    auto rows = (long long) (backdropSpan * bandRepeat)
                + (long long) roundingPerSegment * getSegmentCount();

    sparseBackdrop = (long long) tilesWide * coverageHeight
                     > (long long) sparseBackdropMargin * rows;
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
    bandRuns.clear();
    crossings.clear();

    chooseBackdropForm();

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
            addBackdrop(direction, bandTop, bandBottom, beyond, row);

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

    finishBackdrops();
    countSegmentTests();
}

// One counting sort, into one bucket per tile row, so that a band's crossings
// can be gathered in one pass. The key space is the path's height in tiles, so
// this is linear in the crossings and never in the area.
//
// It sorts the crossings rather than what they expand to, which is the whole
// difference: a crossing covers up to sixteen pixel rows, and ordering the
// expansion instead cost more than the array this replaced.
void PathRasterizer::sortRunsByBand()
{
    sortedRuns.resize(bandRuns.size());
    runCounts.assign(tilesHigh, 0);

    for (const auto& run: bandRuns)
        ++runCounts[run.band];

    auto running = 0;

    for (auto band = 0; band < tilesHigh; ++band)
    {
        auto here = runCounts[band];
        runCounts[band] = running;
        running += here;
    }

    for (const auto& run: bandRuns)
        sortedRuns[runCounts[run.band]++] = run;
}

// The backdrop, in whichever form this path was found to be shaped for. The
// array form is already done: its crossings are what the GPU builds it from, and
// they were recorded as they were found.
void PathRasterizer::finishBackdrops()
{
    if (sparseBackdrop)
        buildStepBackdrop();
}

// Turns the crossings into what a thread looks its backdrop up in: per pixel
// row, the running sum at each tile column the winding changes at.
//
// Only the columns it *changes* at, which is the whole point - a row crossed by
// four edges has four steps whatever its width, so a step list is the outline's
// size and the array it replaces was the area's. A column whose crossings cancel
// changes nothing and is left out with them.
//
// One band at a time, through a scratch array of that band's rows by the path's
// columns. That is the dense array again, and deliberately: at a band's height
// it is a few thousand floats whatever the path covers, it is written once per
// crossing rather than read once per row, and only the columns something landed
// in are summed or cleared. What made the full-sized one expensive was its size,
// not its shape.
//
// Bands are disjoint in rows and taken in order, so each row's steps come out
// after the last row's and nothing is sorted a second time.
void PathRasterizer::buildStepBackdrop()
{
    backdropRows.assign(coverageHeight + 1, 0.f);
    backdropSteps.clear();

    if (!bandRuns.empty())
    {
        sortRunsByBand();

        bandScratch.assign(tileSize * tilesWide, 0.f);
        columnTouched.assign(tilesWide, 0);

        auto at = 0;
        auto count = sortedRuns.size();

        for (auto band = 0; band < tilesHigh; ++band)
        {
            auto bandTop = band * tileSize;
            auto lastRow = std::min(bandTop + tileSize, coverageHeight);

            touchedColumns.clear();

            for (; at < count && sortedRuns[at].band == band; ++at)
            {
                const auto& run = sortedRuns[at];

                columnTouched[run.column] = 1;

                auto firstRow = std::max((int) std::floor(run.fromY), bandTop);
                auto endRow = std::min((int) std::ceil(run.toY), lastRow);

                for (auto row = firstRow; row < endRow; ++row)
                {
                    auto height = std::min(run.toY, (float) (row + 1))
                                  - std::max(run.fromY, (float) row);

                    if (height > 0.f)
                        bandScratch[(row - bandTop) * tilesWide + run.column] +=
                            run.direction * height;
                }
            }

            // Ascending, which is what the kernel's search needs and what the
            // order they were first written in would not give. Collected by
            // walking the columns rather than by sorting what was written,
            // since there is one walk per band and it is the cheaper of the two
            // exactly when the band is full enough for the order to matter.
            for (auto column = 0; column < tilesWide; ++column)
                if (columnTouched[column] != 0)
                    touchedColumns.add(column);

            for (auto row = bandTop; row < lastRow; ++row)
            {
                backdropRows[row] = (float) (backdropSteps.size() / 2);

                const auto* scratch =
                    bandScratch.data() + (row - bandTop) * tilesWide;
                auto running = 0.f;
                auto emitted = 0.f;

                for (auto column: touchedColumns)
                {
                    running += scratch[column];

                    if (running != emitted)
                    {
                        backdropSteps.add((float) column);
                        backdropSteps.add(running);
                        emitted = running;
                    }
                }
            }

            for (auto column: touchedColumns)
            {
                columnTouched[column] = 0;

                for (auto row = 0; row < tileSize; ++row)
                    bandScratch[row * tilesWide + column] = 0.f;
            }
        }
    }

    backdropRows[coverageHeight] = (float) (backdropSteps.size() / 2);

    // A path that changes no row's winding anywhere still binds a buffer, and a
    // zero-length one is not a buffer. No row's run reaches this step, and a
    // guarded read that lands on it is thrown away by the same guard.
    if (backdropSteps.empty())
    {
        backdropSteps.add(0.f);
        backdropSteps.add(0.f);
    }
}

int PathRasterizer::getCellCount() const
{
    return sparseBackdrop ? 0 : tilesWide * coverageHeight;
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
