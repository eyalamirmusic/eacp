#pragma once

#include "BinKernels.h"

namespace eacp::GPUWidgets
{
class PathRasterizer;

// Every path a frame rasterizes, in one set of buffers and one run of
// dispatches.
//
// A rasterizer's own dispatch is priced by the path; a frame's is priced by the
// number of paths, and that is a different cost with a different answer. A canvas
// of a hundred and twenty-eight automation lanes was a hundred and twenty-eight
// dispatches and some five hundred buffer updates, one rasterizer at a time -
// each of which is a bind, a uniform upload and, on D3D12 outside a frame, a
// command list of its own. None of that is the rasterization.
//
// So the paths are concatenated instead: one segment buffer holding every path's
// flattened outline end to end, and a record per path saying where its cells and
// its tiles begin. Every stage finds a thread's path before it does anything else
// and then works exactly as it did.
//
// **The binning and the backdrop are built here, not uploaded.** What the CPU
// sends is the outline; what the GPU makes of it is the count per tile, the
// prefix sum over those counts, the sort of the segments into their tiles and the
// winding entering each tile column - see BinKernels.h and BackdropKernels.h.
// Those stages are per batch and not per path, so a frame pays for them once
// however many paths are in it, and a path drawn on its own is the only thing
// that ever pays all of them by itself.
//
//   batch.begin(atlas.getTexture());
//   for (auto* shape: dirtyShapes)
//       shape->rasterize(atlas, scale, batch);   // CPU only: flatten, and add
//
//   auto compute = frame.beginCompute();
//   batch.dispatch(compute);                     // upload once, then the stages
//
// The coverage stage hands threads out a **block** at a time rather than a pixel
// at a time - the 8x8 the 2D threadgroup already is - so a group's threads are
// one path's eight-by-eight corner, which is the locality the per-path dispatch
// had and the thing a flat pixel index would have thrown away. The grid is that
// many blocks laid out as a rectangle, which is also what keeps a big batch
// inside the 65,535 groups a dimension of a dispatch may have.
//
// Every path added must write into the texture the batch was begun with. Nothing
// else is required of them: sizes and fill rules are per path and stay that way.
class CoverageBatch
{
public:
    CoverageBatch() = default;

    // Empties the batch and names the texture every path in it writes into.
    void begin(const GPU::Texture& targetToUse);

    // Adds a rasterized path: its flattened outline, how much room its binning
    // needs, and where it sits in the target. The rasterizer must have had
    // setPath called and a target set, and must outlive nothing - everything
    // needed is copied here.
    void add(const PathRasterizer& rasterizer);

    bool isEmpty() const { return paths == 0; }
    int getPathCount() const { return paths; }

    // Uploads what was gathered and runs the stages. A no-op on an empty batch,
    // and the pass must end before the render pass that samples the coverage
    // begins.
    void dispatch(GPU::ComputePass& pass);

    // What the last dispatch cost in the units this exists to cut: a handful of
    // dispatches however many paths went in, and one buffer update per buffer
    // that had something in it rather than per path.
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

    // The three arrays the binning stages work in, all of them allocated and
    // none of them uploaded: the backdrop's cells, a count per tile that the
    // prefix sum turns into an offset and then into a cursor, and the segments
    // once they are sorted into their tiles. The only thing the CPU knows about
    // any of them is how big it has to be.
    std::optional<GPU::Buffer> cellBuffer;
    std::optional<GPU::Buffer> tileCountBuffer;
    std::optional<GPU::Buffer> tileOffsetBuffer;
    std::optional<GPU::Buffer> entryBuffer;

    PrefixSum tileSum;

    // The paths' arrays end to end. Kept between frames and refilled, so a
    // canvas whose paths all move re-uploads and allocates nothing.
    Vector<float> segments;
    Vector<float> records;
    Vector<float> blockOffsets;

    // Where each path's run of segments and of scanned pixel rows begins, which
    // is what the binner and the backdrop scan find their own path through.
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

    // Whether what was gathered has reached the buffers. Dispatching the same
    // batch twice is what a rasterizer drawn repeatedly does, and it must upload
    // once: the terminator and the pads the upload appends belong to the arrays
    // exactly once, and re-sending bytes nothing has changed is the cost this
    // whole class is about.
    bool uploaded = false;
};
} // namespace eacp::GPUWidgets
