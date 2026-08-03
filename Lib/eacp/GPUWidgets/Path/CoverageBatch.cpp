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

// Grown by reallocation and refilled in place otherwise: a canvas whose paths all
// move re-uploads the same buffers every frame and allocates nothing after the
// first.
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

// A buffer no path in this batch filled still binds, the kernel declaring all of
// them, and a zero-length buffer is not a buffer. One float rather than a shape:
// no thread reads it, because no record points into it.
void padEmpty(Vector<float>& values)
{
    if (values.empty())
        values.add(0.f);
}

// The arrays the binning and backdrop stages work in. Never uploaded and never
// read back - a kernel is what puts a value in one - so these are allocations
// and nothing else, grown when a batch needs more than the last one did.
void ensureRoom(std::optional<GPU::Buffer>& buffer, std::size_t bytes)
{
    if (!buffer.has_value() || buffer->size() < bytes)
        buffer.emplace(GPU::Device::shared(),
                       nullptr,
                       std::max<std::size_t>(4, bytes),
                       GPU::BufferUsage::Storage);
}

// One path's run onto the end of the batch's. In bulk rather than value by
// value: gathering a canvas is a couple of million floats, and the batch is only
// worth having if putting them together is cheaper than the per-path overhead it
// removes.
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

    // The shape read, which is the only one four of the six stages take: a
    // thread of any of them wants where this path's cells and tiles begin and
    // how big its coverage is, and wants nothing else at all. The binner reads
    // it once per segment, which is why the record is laid out this way round
    // rather than by what the fields mean.
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

    // Every base in a record is a float, which holds an integer exactly to
    // sixteen million and silently rounds one past it. The cells and the tiles
    // are the two that grow with *area* - the rest grow with the outline and
    // would need a batch nothing could draw - and a canvas of a hundred and
    // twenty-eight full-width lanes is already at seven million cells.
    assert(cells <= (1 << 24) && tiles <= (1 << 24)
           && "eacp: a batch's binning outgrew what a float index holds");

    ++paths;
    blocks += blocksWide * blocksHigh;

    // The one place the record's shape is written rather than read, held to the
    // constant every stage reads it through. A field added here and nowhere else
    // does not misread - it shifts every field of every later path by one, which
    // is a picture that looks almost right.
    assert(records.size() == paths * CoverageKernel::recordFloats
           && "eacp: a path record is not CoverageKernel::recordFloats long");
}

void CoverageBatch::upload()
{
    bufferUpdates = 0;

    // The last entry is the total, which is what makes the search's upper bound
    // a read rather than a special case.
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

    // One past the last tile, holding the total: it is what makes the last
    // tile's run end a read of the same array rather than a special case, and
    // what the prefix sum leaves the entry count in.
    ensureRoom(tileCountBuffer, sizeof(std::uint32_t) * (std::size_t) (tiles + 1));
    ensureRoom(tileOffsetBuffer, sizeof(std::uint32_t) * (std::size_t) (tiles + 1));
    ensureRoom(entryBuffer, sizeof(float) * 4 * (std::size_t) entries);
}

// Zero, count, sum, sort - the tiles, for every path in the batch, and the
// backdrop along the way. Each stage orders against the next by the pass itself;
// nothing here needs a fence, and nothing here reaches the CPU.
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

    // The backdrop's crossings landed in the cells on that pass, so this is all
    // that is left of it: the running sum along each pixel row.
    auto& scan = sharedKernel<BackdropScanKernel>();
    scan.records = *recordBuffer;
    scan.cells = *cellBuffer;
    scan.pathStarts = *scanStartBuffer;
    scan.pathCount = paths;
    pass.dispatch(scan, scanRows);
    ++dispatches;

    // Counts into offsets, and the counts themselves back to zero - which is
    // what the sort below hands its slots out with.
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

    // The blocks laid out as a rectangle rather than a row. A dimension of a
    // dispatch may have 65,535 threadgroups, and a canvas of a hundred and
    // twenty-eight full-width lanes is a million and a half blocks - so a single
    // row of them would not be dispatchable at all. Square keeps both sides
    // small and wastes at most one row of blocks past the end, which the guard
    // in the kernel retires.
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
