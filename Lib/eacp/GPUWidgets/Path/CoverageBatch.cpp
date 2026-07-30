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
constexpr auto recordFloats = CoverageKernel::recordFloats;

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

// One kernel for every batch there will ever be. It is the same program in all of
// them, and building one costs a shader library and a compute pipeline.
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

void CoverageBatch::begin(const GPU::Texture& targetToUse)
{
    target = &targetToUse;
    paths = 0;
    blocks = 0;
    uploaded = false;

    segments.clear();
    tileOffsets.clear();
    backdropSteps.clear();
    backdropRows.clear();
    backdrops.clear();
    records.clear();
    blockOffsets.clear();
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

    auto segmentBase = segments.size() / 4;
    auto tileBase = tileOffsets.size();
    auto stepBase = backdropSteps.size() / 2;
    auto rowBase = backdropRows.size();
    auto denseBase = backdrops.size();

    blockOffsets.add((float) blocks);

    records.add((float) segmentBase);
    records.add((float) tileBase);
    records.add((float) stepBase);
    records.add((float) rowBase);

    records.add((float) denseBase);
    records.add((float) width);
    records.add((float) height);
    records.add((float) rasterizer.tilesWide);

    records.add((float) rasterizer.originX);
    records.add((float) rasterizer.originY);
    records.add((float) rasterizer.evenOdd);
    records.add(rasterizer.sparseBackdrop ? 1.f : 0.f);

    appendAll(segments, rasterizer.tileSegments);
    appendAll(tileOffsets, rasterizer.tileOffsets);

    // Only the form this path was built for. The other one holds nothing for it,
    // and nothing reads it: the record says which, and the branch on it is taken
    // one way by every thread of the path.
    if (rasterizer.sparseBackdrop)
    {
        appendAll(backdropSteps, rasterizer.backdropSteps);
        appendAll(backdropRows, rasterizer.backdropRows);
    }
    else
    {
        appendAll(backdrops, rasterizer.backdrops);
    }

    ++paths;
    blocks += blocksWide * blocksHigh;
}

void CoverageBatch::upload()
{
    bufferUpdates = 0;

    // The last entry is the total, which is what makes the search's upper bound
    // a read rather than a special case.
    blockOffsets.add((float) blocks);

    padEmpty(segments);
    padEmpty(tileOffsets);
    padEmpty(backdropSteps);
    padEmpty(backdropRows);
    padEmpty(backdrops);

    uploadTo(segmentBuffer, segments, bufferUpdates);
    uploadTo(tileBuffer, tileOffsets, bufferUpdates);
    uploadTo(stepBuffer, backdropSteps, bufferUpdates);
    uploadTo(rowBuffer, backdropRows, bufferUpdates);
    uploadTo(denseBuffer, backdrops, bufferUpdates);
    uploadTo(recordBuffer, records, bufferUpdates);
    uploadTo(blockBuffer, blockOffsets, bufferUpdates);
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

    // The blocks laid out as a rectangle rather than a row. A dimension of a
    // dispatch may have 65,535 threadgroups, and a canvas of a hundred and
    // twenty-eight full-width lanes is a million and a half blocks - so a single
    // row of them would not be dispatchable at all. Square keeps both sides
    // small and wastes at most one row of blocks past the end, which the guard
    // in the kernel retires.
    gridColumns = (int) std::ceil(std::sqrt((double) blocks));
    auto gridRows = (blocks + gridColumns - 1) / gridColumns;

    auto& kernel = sharedKernel();

    kernel.coverage = *target;
    kernel.segments = *segmentBuffer;
    kernel.tileOffsets = *tileBuffer;
    kernel.backdropSteps = *stepBuffer;
    kernel.backdropRows = *rowBuffer;
    kernel.backdrops = *denseBuffer;
    kernel.records = *recordBuffer;
    kernel.blockOffsets = *blockBuffer;
    kernel.gridColumns = (std::uint32_t) gridColumns;
    kernel.pathCount = paths;

    pass.dispatch(kernel, gridColumns * blockSize, gridRows * blockSize);
    dispatches = 1;
}
} // namespace eacp::GPUWidgets
