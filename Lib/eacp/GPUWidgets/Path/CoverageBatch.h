#pragma once

#include "BinKernels.h"

namespace eacp::GPUWidgets
{
class PathRasterizer;

// Every path a frame rasterizes, concatenated into one set of buffers so the
// binning and backdrop stages run once per batch instead of once per path. The
// CPU sends only the outlines; the tiling is built on the GPU (see BinKernels.h).
class CoverageBatch
{
public:
    CoverageBatch() = default;

    // Empties the batch and names the texture every path in it writes into.
    void begin(const GPU::Texture& targetToUse);

    // The rasterizer must already have a path and a target set, and need not
    // outlive the call: everything is copied.
    void add(const PathRasterizer& rasterizer);

    bool isEmpty() const { return paths == 0; }
    int getPathCount() const { return paths; }

    // A no-op on an empty batch. The pass must end before the render pass that
    // samples the coverage begins.
    void dispatch(GPU::ComputePass& pass);

    int getDispatchCount() const { return dispatches; }
    int getBufferUpdateCount() const { return bufferUpdates; }

private:
    void upload();
    void buildTiles(GPU::ComputePass& pass);

    std::optional<GPU::Buffer> segmentBuffer;
    std::optional<GPU::Buffer> segmentStartBuffer;
    std::optional<GPU::Buffer> scanStartBuffer;
    std::optional<GPU::Buffer> recordBuffer;
    std::optional<GPU::Buffer> blockBuffer;

    // Allocated but never uploaded or read back: the kernels are what fill them.
    std::optional<GPU::Buffer> cellBuffer;
    std::optional<GPU::Buffer> tileCountBuffer;
    std::optional<GPU::Buffer> tileOffsetBuffer;
    std::optional<GPU::Buffer> entryBuffer;

    PrefixSum tileSum;

    // The paths' arrays end to end, kept between frames and refilled in place.
    Vector<float> segments;
    Vector<float> records;
    Vector<float> blockOffsets;

    // Where each path's run of segments and of scanned pixel rows begins.
    Vector<float> segmentStarts;
    Vector<float> scanStarts;

    const GPU::Texture* target = nullptr;
    int paths = 0;
    int blocks = 0;
    int cells = 0;
    int tiles = 0;
    int entries = 0;
    int scanRows = 0;
    int gridColumns = 0;
    int dispatches = 0;
    int bufferUpdates = 0;

    // A batch dispatched twice must upload once: the terminators and pads the
    // upload appends belong to the arrays exactly once.
    bool uploaded = false;
};
} // namespace eacp::GPUWidgets
