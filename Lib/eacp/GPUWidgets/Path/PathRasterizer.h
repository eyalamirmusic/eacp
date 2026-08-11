#pragma once

#include "CoverageBatch.h"
#include "Path.h"

namespace eacp::GPUWidgets
{
// Non-zero fills where the winding number is not zero, even-odd where it is odd.
enum class FillRule
{
    NonZero,
    EvenOdd
};

// Rasterizes a Path's fill into a GPU coverage texture with analytic (not
// multisampled) antialiasing. Coverage accumulates within one path, so its own
// abutting parts do not seam; separate paths are separate draws and still do.
class PathRasterizer
{
public:
    PathRasterizer() = default;

    // Device pixels per path unit; takes effect on the next setPath.
    void setScale(float pixelsPerUnit);
    float getScale() const { return scale; }

    // CPU-only: converts the path to directed segments in coverage pixel space,
    // closing every sub-path. No GPU resource is touched until dispatch.
    void setPath(const Path& path, FillRule rule = FillRule::NonZero);

    // Nothing to dispatch and no coverage to sample.
    bool isEmpty() const;

    int getCoverageWidth() const { return coverageWidth; }
    int getCoverageHeight() const { return coverageHeight; }

    // In path units: the bounds snapped out to whole pixels and grown by one, so
    // an edge on a pixel boundary has somewhere to spill its coverage.
    Graphics::Rect getCoveredBounds() const { return covered; }

    // Closing segments included, horizontal ones dropped.
    int getSegmentCount() const { return segments.size() / 4; }

    // Backdrop cells, one per tile column per pixel row. The only allocation
    // that grows with area.
    int getCellCount() const;

    // Segment-pixel tests the next dispatch will do, predicted from the same
    // arithmetic the binning kernel uses. Walked on demand; only benches read it.
    long long getSegmentTests() const;

    // Entries the counting sort will really produce, against getEntryBound.
    int getEntryCount() const;

    // Room reserved for those entries: a constant-time upper bound, since the
    // exact clip runs on the GPU. Coming up short would silently drop segments.
    int getEntryBound() const { return entryBound; }

    // Writes into a rect of a texture it does not own, given its top-left texel.
    // Unchecked: the texture must be computeWrite, big enough, and outlive the
    // dispatch.
    void setTarget(const GPU::Texture& texture, int originXToUse, int originYToUse);

    // Back to a texture of its own, sized to the path on the next dispatch.
    void clearTarget();

    // One texel per device pixel of getCoveredBounds(), the same value in all
    // four channels. Valid after a dispatch, and only while no target is set.
    const GPU::Texture& getCoverage() const { return *coverageTexture; }

    // A no-op on an empty path. The pass must end before the render pass that
    // samples the coverage begins. This is a batch of one: a frame drawing
    // several paths should hand them to a CoverageBatch instead.
    void dispatch(GPU::ComputePass& pass);

private:
    friend class CoverageBatch;

    // The target texture, or the owned one; null until a dispatch settles which.
    const GPU::Texture* getTargetTexture() const;

    int getTileCount() const { return tilesWide * tilesHigh; }

    void ensureOwnTexture();
    void countTiles() const;

    // A ceiling on the tiles one segment lands in.
    int boundEntriesOf(float fromX, float fromY, float toX, float toY) const;

    // Made on the first solo dispatch, and never for a batch-driven rasterizer.
    std::optional<CoverageBatch> solo;
    std::optional<GPU::Texture> coverageTexture;

    // Directed segments in coverage pixel space, four floats each. Kept between
    // rasterizations so a re-drawn path of the same complexity allocates nothing.
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

    // Negative until countTiles has filled both.
    mutable long long segmentTests = -1;
    mutable int entryCount = 0;

    bool soloStale = true;
};
} // namespace eacp::GPUWidgets
