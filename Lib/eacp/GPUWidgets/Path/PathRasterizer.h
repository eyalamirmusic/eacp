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
// Segments are binned into tiles so a thread walks the outline near its own
// pixel rather than the whole of it. What that costs is the sum over tiles of
// tile pixels times the segments crossing the tile, plus one backdrop lookup for
// every pixel that has no outline near it at all - getSegmentTests() reports the
// first term. A path is therefore priced by its outline rather than by its area,
// which is what lets one cover the window.
//
// **None of that happens here.** What this class does is flatten the path and
// transform it into the coverage texture's pixel space, which is `O(segments)`
// and nothing else. The clip that decides which tiles each segment lands in, the
// count per tile, the prefix sum over them and the counting sort that files the
// segments are four kernels - see BinKernels.h - and so is the backdrop, which
// falls out of the same clip. The only thing settled on this side is how much
// room to allocate, which is a bound taken per segment without clipping.
class PathRasterizer
{
public:
    PathRasterizer() = default;

    // Device pixels per path unit - a view's backingScale, times whatever zoom
    // the path is drawn at. Takes effect on the next setPath.
    void setScale(float pixelsPerUnit);
    float getScale() const { return scale; }

    // Flattens the path into directed segments in coverage pixel space and
    // measures the room its rasterization will need. Every sub-path is closed,
    // since a fill always treats one as closed. Nothing on the GPU is touched
    // here - no texture, and no buffer: the bytes go up when the work is
    // recorded, which is what lets a frame send every path's at once and, on
    // D3D12, put the copies on the list the frame is already building.
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

    // What this path's backdrop costs a batch in cells - one integer per tile
    // column per pixel row. This is the only thing a batch allocates that grows
    // with *area*, so it is the only one that can reach a ceiling.
    int getCellCount() const;

    // Segment-pixel tests the next dispatch will do, which is the work binning
    // exists to cut: the same path unbinned costs coverage width times height
    // times getSegmentCount().
    //
    // Counted when asked for and not when the path is set - it is the one thing
    // here that still does the clip on the CPU, and nothing but a bench and a
    // test ever reads it. What it counts is what the binning kernel will find,
    // from the same arithmetic; it is a prediction rather than a measurement,
    // and the only place the two could differ is a boundary an ulp wide.
    long long getSegmentTests() const;

    // How many times a segment appears under some tile, which is the size of the
    // array the counting sort fills. The dispatch is sized by a bound on this
    // rather than by this - see getEntryBound - so the two together are what say
    // whether that bound is one.
    int getEntryCount() const;

    // The room a batch reserves for those entries. The exact count is what the
    // clip finds, and the clip is on the GPU, so this is an upper bound taken
    // per segment in constant time - see measure(). It has to be one: a bound
    // that came up short would drop segments and say nothing.
    int getEntryBound() const { return entryBound; }

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
    // use it: hand them all to a CoverageBatch instead and dispatch that. What it
    // costs over the batch is a set of buffers of its own and, more than that,
    // the stages - clearing, binning, summing and sorting are per batch, so a
    // path on its own pays for all of them by itself. That is the right trade for
    // the demo and the test that rasterize a single path and the wrong one for an
    // interface.
    void dispatch(GPU::ComputePass& pass);

private:
    friend class CoverageBatch;

    // Where the coverage goes: the texture a slot was taken in, or the one this
    // owns. Null until a dispatch has settled which.
    const GPU::Texture* getTargetTexture() const;

    // Tiles across the coverage, which is what a batch adds up to size the count
    // and offset arrays the binning kernels work in.
    int getTileCount() const { return tilesWide * tilesHigh; }

    void ensureOwnTexture();
    void countTiles() const;

    // A ceiling on the tiles one segment lands in, taken as the segment is
    // emitted - see the definition for why that is where it belongs.
    int boundEntriesOf(float fromX, float fromY, float toX, float toY) const;

    // The buffers a solo dispatch needs, made on the first one and not at all
    // for a rasterizer a CoverageBatch drives - which is every one in an
    // interface.
    std::optional<CoverageBatch> solo;
    std::optional<GPU::Texture> coverageTexture;

    // Directed segments in coverage pixel space, four floats each. The one array
    // this builds, and the only thing it hands a batch. Kept between
    // rasterizations, so a path re-drawn at the same complexity allocates
    // nothing at all.
    Vector<float> segments;

    const GPU::Texture* target = nullptr;
    int originX = 0;
    int originY = 0;

    Graphics::Rect covered;
    float scale = 1.f;
    int coverageWidth = 0;
    int coverageHeight = 0;
    int tilesWide = 0;
    int tilesHigh = 0;
    int entryBound = 0;
    int evenOdd = 0;

    // Negative until something asks, which is what makes the walk that fills
    // both of them lazy.
    mutable long long segmentTests = -1;
    mutable int entryCount = 0;

    // Whether the solo batch still describes this path. A rasterizer dispatched
    // repeatedly without changing - which is what a benchmark and a static demo
    // do - gathers and uploads once.
    bool soloStale = true;
};
} // namespace eacp::GPUWidgets
