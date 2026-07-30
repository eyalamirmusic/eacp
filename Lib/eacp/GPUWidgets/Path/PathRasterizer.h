#pragma once

#include "CoverageBatch.h"
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
// backdrop lookup for every pixel that has no outline near it at all -
// getSegmentTests() reports the first term. A path is therefore priced by its
// outline rather than by its area, which is what lets one cover the window.
//
// What everything to the left of a tile contributes - the backdrop - can be
// priced by the outline too, and that is the one thing here that is not obvious.
// A pixel row's backdrop is the running sum of the winding entering it at each
// tile column, so it is a *step function*, and its steps are the outline's
// crossings of that row rather than the row's columns: a window-sized ellipse
// has five steps across two hundred columns, where holding a value per column
// per row cost more CPU than everything else here put together.
//
// Steps are two numbers where a column is one, though, so an outline crossing
// nearly every row of its own coverage is cheaper held the plain way. Both are
// built, one per path, whichever this one is shaped for.
class PathRasterizer
{
public:
    PathRasterizer() = default;

    // Device pixels per path unit - a view's backingScale, times whatever zoom
    // the path is drawn at. Takes effect on the next setPath.
    void setScale(float pixelsPerUnit);
    float getScale() const { return scale; }

    // Bins the path's segments and measures the coverage it will need. Every
    // sub-path is closed, since a fill always treats one as closed. Nothing on
    // the GPU is touched here - no texture, and no buffer: the bytes go up when
    // the work is recorded, which is what lets a frame send every path's at once
    // and, on D3D12, put the copies on the list the frame is already building.
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
    //
    // This is a batch of one, and a frame drawing more than one path should not
    // use it: hand them all to a CoverageBatch instead and dispatch that. What
    // this costs over the batch is a set of buffers of its own, which is the
    // right trade for the demo and the test that rasterize a single path and the
    // wrong one for an interface.
    void dispatch(GPU::ComputePass& pass);

private:
    friend class CoverageBatch;

    // Where the coverage goes: the texture a slot was taken in, or the one this
    // owns. Null until a dispatch has settled which.
    const GPU::Texture* getTargetTexture() const;

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

    // One segment's crossing into one tile column, over the part of one tile
    // row's band it spans. The winding it hands the pixel rows to the right of
    // it, kept as the crossing rather than expanded over those rows - there is
    // one of these per segment per band, and expanding gives sixteen.
    struct BandRun
    {
        int band;
        int column;
        float fromY;
        float toY;
        float direction;
    };

    void ensureOwnTexture();
    void buildTiles();
    void chooseBackdropForm();
    void buildDenseBackdrop();
    void buildStepBackdrop();
    void addBackdrop(float direction, float fromY, float toY, int column, int band);
    void sortRunsByBand();
    void finishBackdrops();
    void countSegmentTests();

    // The buffers a solo dispatch needs, made on the first one and not at all
    // for a rasterizer a CoverageBatch drives - which is every one in an
    // interface.
    std::optional<CoverageBatch> solo;
    std::optional<GPU::Texture> coverageTexture;

    // Directed segments in coverage pixel space, four floats each, before
    // binning. Every buffer below is kept between rasterizations too, so a path
    // re-drawn at the same complexity allocates nothing at all.
    Vector<float> segments;

    Vector<TileRun> runs;
    Vector<float> tileOffsets;
    Vector<int> tileCursor;
    Vector<float> tileSegments;

    Vector<BandRun> bandRuns;
    Vector<BandRun> sortedRuns;
    Vector<int> runCounts;

    // One band's winding per pixel row per tile column, which is the dense
    // array this replaced - at one band's size rather than the path's, and
    // cleared where it was written rather than all over.
    Vector<float> bandScratch;
    Vector<char> columnTouched;
    Vector<int> touchedColumns;

    // The backdrop as the kernel reads it, in whichever of the two forms is
    // smaller for this path: a run of (column, winding) steps per pixel row with
    // where each row's run starts, or the winding at every column of every row.
    Vector<float> backdropSteps;
    Vector<float> backdropRows;
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
    bool sparseBackdrop = true;
    long long segmentTests = 0;
    float backdropSpan = 0.f;

    // Whether the solo batch still describes this path. A rasterizer dispatched
    // repeatedly without changing - which is what a benchmark and a static demo
    // do - gathers and uploads once.
    bool soloStale = true;
};
} // namespace eacp::GPUWidgets
