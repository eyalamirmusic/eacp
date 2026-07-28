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

// Grown by reallocation and refilled in place otherwise: a path re-drawn at the
// same complexity - a knob turning, a curve dragged - reuses every buffer it
// already has.
void uploadTo(std::optional<GPU::Buffer>& buffer, const Vector<float>& values)
{
    auto bytes = sizeof(float) * (std::size_t) values.size();

    if (!buffer.has_value() || buffer->size() < bytes)
        buffer.emplace(
            GPU::Device::shared(), values.data(), bytes, GPU::BufferUsage::Storage);
    else
        buffer->update(values.data(), bytes);
}

// One kernel for every rasterizer there will ever be. It is the same program in
// all of them, and building one costs a shader library and a compute pipeline -
// which an interface with a rasterizer per widget must not pay per widget.
//
// Shared state is safe here because a dispatch sets every uniform it reads
// immediately before issuing it, and command encoding is single-threaded. Built
// on first use rather than at load, since it needs the Device - which also puts
// its destruction before the Device's own, statics tearing down in reverse.
CoverageKernel& sharedKernel()
{
    struct Prepared
    {
        Prepared() { kernel.prepare(); }

        CoverageKernel kernel;
    };

    static auto prepared = Prepared {};
    return prepared.kernel;
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

    // Its own texture is now dead weight, and holding it would keep a whole
    // path's worth of pixels alive for as long as the shape exists.
    coverageTexture.reset();
}

void PathRasterizer::clearTarget()
{
    target = nullptr;
    originX = 0;
    originY = 0;
}

void PathRasterizer::setPath(const Path& path, FillRule rule)
{
    segments.clear();
    covered = {};
    coverageWidth = 0;
    coverageHeight = 0;
    segmentTests = 0;

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
    upload();
}

void PathRasterizer::addBackdrop(float direction, float fromY, float toY, int column)
{
    if (column >= tilesWide)
        return;

    auto firstRow = std::max((int) std::floor(fromY), 0);
    auto lastRow = std::min((int) std::ceil(toY), coverageHeight);

    for (auto row = firstRow; row < lastRow; ++row)
    {
        auto height =
            std::min(toY, (float) (row + 1)) - std::max(fromY, (float) row);

        if (height > 0.f)
            backdrops[row * tilesWide + column] += direction * height;
    }
}

// Splits every segment across the tile rows it crosses, and within each row
// files it under the tiles it actually reaches. Clipping to the row first is
// what keeps a long diagonal honest: binned by its bounding box it would land in
// every tile of a square, when it passes through a couple per row.
//
// Everything to the left of the tiles it reaches is not filed anywhere. It
// covers those pixels' whole row-slices, so its contribution turns on the pixel
// row alone, and it is summed into the backdrop instead - a number a thread
// starts its winding from rather than a list it walks.
void PathRasterizer::buildTiles()
{
    tilesWide = (coverageWidth + tileSize - 1) / tileSize;
    tilesHigh = (coverageHeight + tileSize - 1) / tileSize;

    auto tileCount = tilesWide * tilesHigh;

    runs.clear();
    tileOffsets.assign(tileCount + 1, 0.f);
    backdrops.assign(tilesWide * coverageHeight, 0.f);

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
            addBackdrop(direction, bandTop, bandBottom, beyond);

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

// Each column held only what crossed into it; running them left to right turns
// that into the winding entering the column, which is what a thread reads.
void PathRasterizer::finishBackdrops()
{
    for (auto row = 0; row < coverageHeight; ++row)
    {
        auto* columns = backdrops.data() + row * tilesWide;

        for (auto column = 1; column < tilesWide; ++column)
            columns[column] += columns[column - 1];
    }
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

void PathRasterizer::upload()
{
    uploadTo(segmentBuffer, tileSegments);
    uploadTo(tileBuffer, tileOffsets);
    uploadTo(backdropBuffer, backdrops);
}

void PathRasterizer::dispatch(GPU::ComputePass& pass)
{
    if (isEmpty())
        return;

    if (target == nullptr)
        ensureOwnTexture();

    auto& kernel = sharedKernel();

    kernel.coverage = target != nullptr ? *target : *coverageTexture;
    kernel.segments = *segmentBuffer;
    kernel.tileOffsets = *tileBuffer;
    kernel.backdrops = *backdropBuffer;
    kernel.tilesWide = (std::uint32_t) tilesWide;
    kernel.evenOdd = evenOdd;
    kernel.originX = (std::uint32_t) originX;
    kernel.originY = (std::uint32_t) originY;

    pass.dispatch(kernel, coverageWidth, coverageHeight);
}
} // namespace eacp::GPUWidgets
