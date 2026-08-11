#include "PathShape.h"

#include "../Component/Component.h"
#include "ContentHash.h"

namespace eacp::UI
{
namespace
{
// Only what reaches the kernel: the flattened points, the sub-path boundaries
// and the fill rule.
std::uint64_t hashGeometry(const GPUWidgets::Path& path, GPUWidgets::FillRule rule)
{
    auto hash = ContentHash {};

    hash.mix((int) rule);

    for (const auto& subPath: path.getSubPaths())
    {
        // The count too, so one sub-path and two holding the same points in the
        // same order hash differently.
        hash.mix((int) subPath.points.size());
        hash.mix(subPath.closed);

        for (const auto& point: subPath.points)
        {
            hash.mix(point.x);
            hash.mix(point.y);
        }
    }

    return hash.get();
}

// Where Backing::Automatic switches over, in device pixels of mask - a fraction
// of a full atlas rather than a size in points, the atlas being what it guards.
constexpr auto meshTexelThreshold = 256.f * 256.f;
} // namespace

PathShape::PathShape(Component& ownerToUse)
    : owner(ownerToUse)
{
    owner.addPathShape(*this);
}

PathShape::~PathShape()
{
    owner.removePathShape(*this);

    // The owner's recorded quad carries this shape's uv, and the slot behind it
    // goes to whoever the atlas hands it to next.
    owner.repaint();
}

void PathShape::setPath(const GPUWidgets::Path& newPath, GPUWidgets::FillRule rule)
{
    auto hash = hashGeometry(newPath, rule);

    // The same shape, rebuilt by the same arithmetic: the mask and the quad both
    // still hold. An empty path is always taken, a hash not describing that one.
    if (!path.isEmpty() && hash == geometryHash)
        return;

    path = newPath;
    fillRule = rule;
    geometryHash = hash;
    dirty = true;

    // The owner recorded a quad of the old bounds sampling the old atlas rect.
    owner.repaint();
}

void PathShape::setStroke(const GPUWidgets::Path& newPath,
                          const GPUWidgets::StrokeStyle& style)
{
    // Non-zero and not the caller's choice: a stroke reaches the kernel as
    // overlapping pieces, and under even-odd every overlap would read as a hole.
    setPath(GPUWidgets::strokeToFill(newPath, style), GPUWidgets::FillRule::NonZero);
}

void PathShape::setBacking(Backing newBacking)
{
    if (backing == newBacking)
        return;

    backing = newBacking;
    dirty = true;

    owner.repaint();
}

void PathShape::clear()
{
    path.clear();
    geometryHash = 0;
    mesh.clear();
    dirty = true;
    ready = false;
    dropped = false;
    bounds = {};

    owner.repaint();
}

bool PathShape::isEmpty() const
{
    return !ready;
}

Rect PathShape::getBounds() const
{
    return bounds;
}

// Feathers by one device pixel, expressed in the path's own points.
bool PathShape::buildMesh(float scale)
{
    auto pathBounds = path.getBounds();

    if (backing == Backing::Automatic
        && pathBounds.w * scale * pathBounds.h * scale < meshTexelThreshold)
        return false;

    auto feather = 1.f / scale;

    mesh = GPUWidgets::tessellateAntialiasedFill(path, feather);

    if (mesh.empty())
        return false;

    auto half = feather * 0.5f;

    bounds = {pathBounds.x - half,
              pathBounds.y - half,
              pathBounds.w + feather,
              pathBounds.h + feather};

    ready = true;

    return true;
}

void PathShape::rasterize(CoverageAtlas& atlas,
                          MaskCache& cache,
                          float scale,
                          GPUWidgets::CoverageBatch& batch)
{
    dirty = false;
    ready = false;
    dropped = false;
    sharing = false;
    mesh.clear();

    if (path.isEmpty() || scale <= 0.f)
        return;

    // Tried first: it takes no atlas slot at all when it works.
    if (backing != Backing::Mask && buildMesh(scale))
        return;

    // What this shape published is no longer what it draws. Taken back while
    // nobody shared it; when somebody did, the slot is theirs for good.
    if (publishedKey != 0 && publishedKey != geometryHash)
    {
        if (!cache.reclaim(publishedKey))
            placed = false;

        publishedKey = 0;
        churning = true;
    }

    if (const auto* alreadyRasterized = cache.take(geometryHash, path, fillRule))
    {
        // Somebody else's texels: no dispatch, and no claim on the slot.
        slot = alreadyRasterized->slot;
        maskUV = alreadyRasterized->maskUV;
        bounds = alreadyRasterized->bounds;
        placed = false;
        ready = true;
        sharing = true;

        return;
    }

    rasterizer.setScale(scale);
    rasterizer.setPath(path, fillRule);

    if (rasterizer.isEmpty())
        return;

    auto width = rasterizer.getCoverageWidth();
    auto height = rasterizer.getCoverageHeight();

    // The room already reserved is kept whenever the new mask fits it, so an
    // animating shape whose mask only shrinks allocates nothing.
    if (!placed || width > slot.width || height > slot.height)
    {
        slot = atlas.allocate(width, height);
        placed = slot.width > 0;

        // The atlas is at its ceiling; the shape draws as nothing until it is
        // rebuilt, which invalidates every shape.
        if (!placed)
        {
            dropped = true;

            // Cleared, so a caller asking where the shape is - a clip narrowing
            // itself to the bounds - is told nowhere rather than the old size.
            bounds = {};
            return;
        }
    }

    rasterizer.setTarget(atlas.getTexture(), slot.x, slot.y);
    batch.add(rasterizer);

    maskUV = atlas.uvFor(slot.x, slot.y, width, height);
    bounds = rasterizer.getCoveredBounds();
    ready = true;

    // Only a shape whose geometry has never changed publishes: an animating one
    // wants this slot again next frame.
    if (!churning)
    {
        cache.publish(geometryHash, path, fillRule, {slot, maskUV, bounds});
        publishedKey = geometryHash;
    }
}
} // namespace eacp::UI
