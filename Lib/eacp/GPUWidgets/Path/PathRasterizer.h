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
// What it costs is bounding-box pixels times segments: a 64x64 icon of 200
// segments is under a million segment-pixel tests and free, a full-screen path
// of ten thousand segments is twenty billion and hopeless. This is for
// UI-scale paths, and binning segments into tiles is what lifts that ceiling.
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
    void ensureOwnTexture();
    void uploadSegments();

    std::optional<GPU::Buffer> segmentBuffer;
    std::optional<GPU::Texture> coverageTexture;

    // Directed segments in coverage pixel space, four floats each. Kept between
    // uploads so a re-rasterization at the same size reuses the buffer.
    Vector<float> segments;

    const GPU::Texture* target = nullptr;
    int originX = 0;
    int originY = 0;

    Graphics::Rect covered;
    float scale = 1.f;
    int coverageWidth = 0;
    int coverageHeight = 0;
    int segmentCount = 0;
    int evenOdd = 0;
};
} // namespace eacp::GPUWidgets
