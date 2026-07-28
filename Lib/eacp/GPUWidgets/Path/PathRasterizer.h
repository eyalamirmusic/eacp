#pragma once

#include "CoverageKernel.h"
#include "Path.h"

namespace eacp::GPUWidgets
{
// How a path's interior is decided where its contours overlap: non-zero fills
// wherever the winding number is not zero, even-odd wherever it is odd. They
// differ only on a self-intersecting outline or on nested contours wound the
// same way - a five-pointed star drawn in one stroke is solid under the first
// and hollow-centred under the second.
enum class FillRule
{
    NonZero,
    EvenOdd
};

// Rasterizes a Path's fill into a coverage texture on the GPU, computing the
// antialiasing analytically rather than approximating it by multisampling.
//
// Every pixel gets the exact fraction of itself the path covers, so an edge is
// as smooth as an 8-bit channel can hold it - well past what any practical MSAA
// count reaches - and coverage accumulates *within* the path, so two of its own
// abutting parts meet with no seam between them. Abutting separate paths are
// separate draws and still seam; that is a different problem and unaffected by
// this.
//
//   PathRasterizer rasterizer;
//   rasterizer.setScale(view.backingScale());
//   rasterizer.setPath(star, FillRule::EvenOdd);
//   ...
//   {
//       auto compute = frame.beginCompute();
//       rasterizer.dispatch(compute);
//   }
//   auto pass = frame.beginPass();          // samples rasterizer.getCoverage()
//
// By default it owns the texture it writes, sized to the path. setTarget points
// it at a rect of a texture it does not own instead, which is what lets a whole
// interface's paths share one atlas and draw in one batch - see
// UI::CoverageAtlas.
//
// Segments are binned into tiles here, on the CPU, so a thread walks the outline
// near its own pixel rather than the whole of it. What that costs is the sum
// over tiles of tile pixels times the segments crossing the tile, plus one
// backdrop read for every pixel that has no outline near it at all -
// getSegmentTests() reports the first term. A path is therefore priced by its
// outline rather than by its area, which is what lets one cover the window.
class PathRasterizer
{
public:
    PathRasterizer() = default;

    // Device pixels per path unit - a view's backingScale, times whatever zoom
    // the path is drawn at. Takes effect on the next setPath.
    void setScale(float pixelsPerUnit);
    float getScale() const { return scale; }

    // Uploads the path's segments and measures the coverage it will need. Every
    // sub-path is closed, since a fill always treats one as closed. No texture
    // is touched here: the size it settles is what a caller asks an atlas for
    // before pointing this at a slot of it.
    void setPath(const Path& path, FillRule rule = FillRule::NonZero);

    // True when there is nothing to dispatch and no coverage to sample - an
    // empty path, or one whose bounds hold no pixels at this scale.
    bool isEmpty() const;

    int getCoverageWidth() const { return coverageWidth; }
    int getCoverageHeight() const { return coverageHeight; }

    // The rect in path units the coverage spans: the path's bounds snapped out
    // to whole pixels and grown by one, so an edge landing on a pixel boundary
    // still has somewhere to spill its coverage.
    Graphics::Rect getCoveredBounds() const { return covered; }

    // Directed segments the flattened path came to, closing ones included and
    // horizontal ones dropped. How complex this path is, in the only unit the
    // kernel counts in.
    int getSegmentCount() const { return segments.size() / 4; }

    // Segment-pixel tests the next dispatch will do, which is the work binning
    // exists to cut: the same path unbinned costs coverage width times height
    // times getSegmentCount(). Settled by setPath, so it can be read before a
    // frame rather than measured during one.
    long long getSegmentTests() const { return segmentTests; }

    // Writes into a rect of someone else's texture, whose top-left texel is
    // given. The texture must have been created with computeWrite, must outlive
    // the dispatch, and must be large enough - nothing here checks, because the
    // allocator that handed out the rect already did.
    void setTarget(const GPU::Texture& texture, int originXToUse, int originYToUse);

    // Back to a texture of its own, sized to the path on the next dispatch.
    void clearTarget();

    // The coverage mask, one texel per device pixel of getCoveredBounds(), with
    // the same value in all four channels. Valid after a dispatch, and only
    // while no target is set - with one, the coverage belongs to the target.
    const GPU::Texture& getCoverage() const { return *coverageTexture; }

    // Runs the kernel over the covered rect. A no-op on an empty path. The pass
    // must end before the render pass that samples the coverage begins.
    void dispatch(GPU::ComputePass& pass);

private:
    // One segment's crossing of one tile row: the tiles it lands in are a run,
    // because within a row a straight segment spans a contiguous range of
    // columns. Recorded on the counting pass so the filling pass does not clip
    // the geometry a second time.
    struct TileRun
    {
        int segment;
        int firstTile;
        int tiles;
    };

    void ensureOwnTexture();
    void buildTiles();
    void addBackdrop(float direction, float fromY, float toY, int column);
    void finishBackdrops();
    void countSegmentTests();
    void upload();

    std::optional<GPU::Buffer> segmentBuffer;
    std::optional<GPU::Buffer> tileBuffer;
    std::optional<GPU::Buffer> backdropBuffer;
    std::optional<GPU::Texture> coverageTexture;

    // Directed segments in coverage pixel space, four floats each, before
    // binning. Every buffer below is kept between rasterizations too, so a path
    // re-drawn at the same complexity allocates nothing at all.
    Vector<float> segments;

    Vector<TileRun> runs;
    Vector<float> tileOffsets;
    Vector<int> tileCursor;
    Vector<float> tileSegments;
    Vector<float> backdrops;

    const GPU::Texture* target = nullptr;
    int originX = 0;
    int originY = 0;

    Graphics::Rect covered;
    float scale = 1.f;
    int coverageWidth = 0;
    int coverageHeight = 0;
    int tilesWide = 0;
    int tilesHigh = 0;
    int evenOdd = 0;
    long long segmentTests = 0;
};
} // namespace eacp::GPUWidgets
