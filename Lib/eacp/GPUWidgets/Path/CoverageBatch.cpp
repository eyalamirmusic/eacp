#include "CoverageBatch.h"

#include "PathRasterizer.h"

#include <cassert>
#include <cmath>
#include <cstring>

namespace eacp::GPUWidgets
{
namespace
{
constexpr auto blockSize = CoverageKernel::blockSize;

// Reallocates only to grow; otherwise refills in place.
void uploadTo(std::optional<GPU::Buffer>& buffer,
              const Vector<float>& values,
              int& updates)
{
    auto bytes = sizeof(float) * (std::size_t) values.size();

    if (!buffer.has_value() || buffer->size() < bytes)
        buffer.emplace(
            GPU::Device::shared(), values.data(), bytes, GPU::BufferUsage::Storage);
    else
        buffer->update(values.data(), bytes);

    ++updates;
}

// The kernel binds every buffer it declares, and a zero-length one is invalid.
// No thread reads the pad, since no record points into it.
void padEmpty(Vector<float>& values)
{
    if (values.empty())
        values.add(0.f);
}

// An allocation and nothing else: these buffers are never uploaded or read back.
void ensureRoom(std::optional<GPU::Buffer>& buffer, std::size_t bytes)
{
    if (!buffer.has_value() || buffer->size() < bytes)
        buffer.emplace(GPU::Device::shared(),
                       nullptr,
                       std::max<std::size_t>(4, bytes),
                       GPU::BufferUsage::Storage);
}

// In bulk: gathering a canvas is a couple of million floats.
void appendAll(Vector<float>& into, const Vector<float>& from)
{
    if (from.empty())
        return;

    auto at = into.size();
    into.resize(at + from.size());

    std::memcpy(
        into.data() + at, from.data(), sizeof(float) * (std::size_t) from.size());
}
} // namespace

void CoverageBatch::begin(const GPU::Texture& targetToUse)
{
    target = &targetToUse;
    paths = 0;
    blocks = 0;
    cells = 0;
    tiles = 0;
    entries = 0;
    scanRows = 0;
    uploaded = false;

    segments.clear();
    records.clear();
    blockOffsets.clear();
    segmentStarts.clear();
    scanStarts.clear();
}

void CoverageBatch::add(const PathRasterizer& rasterizer)
{
    assert(target != nullptr && "eacp: CoverageBatch::begin before add");

    if (rasterizer.isEmpty())
        return;

    assert(rasterizer.getTargetTexture() == target
           && "eacp: every path in a batch writes into the batch's own texture");

    auto width = rasterizer.coverageWidth;
    auto height = rasterizer.coverageHeight;
    auto blocksWide = (width + blockSize - 1) / blockSize;
    auto blocksHigh = (height + blockSize - 1) / blockSize;

    blockOffsets.add((float) blocks);
    segmentStarts.add((float) (segments.size() / 4));
    scanStarts.add((float) scanRows);

    // Ordered by what the binner reads once per segment, not by meaning.
    records.add((float) cells);
    records.add((float) width);
    records.add((float) height);
    records.add((float) tiles);

    records.add((float) rasterizer.evenOdd);
    records.add((float) rasterizer.originX);
    records.add((float) rasterizer.originY);
    records.add(0.f);

    appendAll(segments, rasterizer.segments);

    cells += rasterizer.getCellCount();
    tiles += rasterizer.getTileCount();
    entries += rasterizer.getEntryBound();
    scanRows += height;

    // A record base is a float, exact to 2^24 and silently rounded past it.
    assert(cells <= (1 << 24) && tiles <= (1 << 24)
           && "eacp: a batch's binning outgrew what a float index holds");

    ++paths;
    blocks += blocksWide * blocksHigh;

    // A field added here alone shifts every later path's record by one.
    assert(records.size() == paths * CoverageKernel::recordFloats
           && "eacp: a path record is not CoverageKernel::recordFloats long");
}

void CoverageBatch::upload()
{
    bufferUpdates = 0;

    // A terminating total, so the last path's run end is a read like any other.
    blockOffsets.add((float) blocks);
    segmentStarts.add((float) (segments.size() / 4));
    scanStarts.add((float) scanRows);

    padEmpty(segments);

    uploadTo(segmentBuffer, segments, bufferUpdates);
    uploadTo(segmentStartBuffer, segmentStarts, bufferUpdates);
    uploadTo(scanStartBuffer, scanStarts, bufferUpdates);
    uploadTo(recordBuffer, records, bufferUpdates);
    uploadTo(blockBuffer, blockOffsets, bufferUpdates);

    ensureRoom(cellBuffer, sizeof(std::uint32_t) * (std::size_t) cells);

    // One past the last tile holds the total the prefix sum ends with.
    ensureRoom(tileCountBuffer, sizeof(std::uint32_t) * (std::size_t) (tiles + 1));
    ensureRoom(tileOffsetBuffer, sizeof(std::uint32_t) * (std::size_t) (tiles + 1));
    ensureRoom(entryBuffer, sizeof(float) * 4 * (std::size_t) entries);
}

// Zero, count, sum, sort. The pass orders each stage against the next.
void CoverageBatch::buildTiles(GPU::ComputePass& pass)
{
    auto& clear = sharedKernel<ClearKernel>();
    clear.cells = *cellBuffer;
    clear.tileCounts = *tileCountBuffer;
    clear.cellCount = (std::uint32_t) cells;
    clear.tileCount = (std::uint32_t) (tiles + 1);
    pass.dispatch(clear, std::max(cells, tiles + 1));
    ++dispatches;

    auto& bin = sharedKernel<BinKernel>();
    bin.segments = *segmentBuffer;
    bin.records = *recordBuffer;
    bin.pathStarts = *segmentStartBuffer;
    bin.cells = *cellBuffer;
    bin.tileCounts = *tileCountBuffer;
    bin.tileOffsets = *tileOffsetBuffer;
    bin.tileSegments = *entryBuffer;
    bin.entryCapacity = (std::uint32_t) entries;
    bin.pathCount = paths;

    auto segmentCount = segments.size() / 4;

    bin.mode = BinKernel::countMode;
    pass.dispatch(bin, segmentCount);
    ++dispatches;

    // The count pass left the backdrop's crossings in the cells; sum each row.
    auto& scan = sharedKernel<BackdropScanKernel>();
    scan.records = *recordBuffer;
    scan.cells = *cellBuffer;
    scan.pathStarts = *scanStartBuffer;
    scan.pathCount = paths;
    pass.dispatch(scan, scanRows);
    ++dispatches;

    // Counts become offsets and the counts reset to zero, the sort's cursors.
    tileSum.run(pass, *tileCountBuffer, *tileOffsetBuffer, tiles + 1);
    dispatches += tileSum.getDispatchCount();

    bin.mode = BinKernel::fillMode;
    pass.dispatch(bin, segmentCount);
    ++dispatches;
}

void CoverageBatch::dispatch(GPU::ComputePass& pass)
{
    dispatches = 0;
    bufferUpdates = 0;

    if (isEmpty() || target == nullptr)
        return;

    if (!uploaded)
    {
        upload();
        uploaded = true;
    }

    buildTiles(pass);

    // Square rather than a row: a dispatch dimension holds 65,535 threadgroups.
    // The kernel's guard retires the blocks past the end.
    gridColumns = (int) std::ceil(std::sqrt((double) blocks));
    auto gridRows = (blocks + gridColumns - 1) / gridColumns;

    auto& kernel = sharedKernel<CoverageKernel>();

    kernel.coverage = *target;
    kernel.tileSegments = *entryBuffer;
    kernel.tileOffsets = *tileOffsetBuffer;
    kernel.cells = *cellBuffer;
    kernel.records = *recordBuffer;
    kernel.pathStarts = *blockBuffer;
    kernel.gridColumns = (std::uint32_t) gridColumns;
    kernel.pathCount = paths;

    pass.dispatch(kernel, gridColumns * blockSize, gridRows * blockSize);
    ++dispatches;
}
} // namespace eacp::GPUWidgets
