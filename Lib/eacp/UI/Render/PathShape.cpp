#include "PathShape.h"

#include "../Component/Component.h"

namespace eacp::UI
{
namespace
{
// Where Backing::Automatic switches over, in device pixels of mask.
//
// It is a fraction of the atlas rather than a size in points, because what it
// guards is the atlas: at 256 by 256 a shape is a 256th of a full one, so a tree
// would need that many of them before the ceiling came into view, and anything
// larger is a shape whose mask is worth more than its edge quality. A widget's
// paths are far below it -- a knob's indicator at any size an interface uses is
// a few thousand texels -- so an interface is untouched by this and artwork is
// what meets it.
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
}

void PathShape::setPath(const GPUWidgets::Path& newPath, GPUWidgets::FillRule rule)
{
    path = newPath;
    fillRule = rule;
    dirty = true;
}

void PathShape::setStroke(const GPUWidgets::Path& newPath,
                          const GPUWidgets::StrokeStyle& style)
{
    // Non-zero and not the caller's choice: a stroke reaches the kernel as
    // overlapping pieces, and under even-odd every overlap between them would
    // read as a hole.
    setPath(GPUWidgets::strokeToFill(newPath, style), GPUWidgets::FillRule::NonZero);
}

void PathShape::setBacking(Backing newBacking)
{
    if (backing == newBacking)
        return;

    backing = newBacking;
    dirty = true;
}

void PathShape::clear()
{
    path.clear();
    mesh.clear();
    dirty = true;
    ready = false;
    dropped = false;
    bounds = {};
}

bool PathShape::isEmpty() const
{
    return !ready;
}

Rect PathShape::getBounds() const
{
    return bounds;
}

// A feather a device pixel wide, expressed in the path's own points: the widest
// ramp that still reads as an edge rather than a blur, which is the same
// judgement the distance-field shapes make.
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
                          float scale,
                          GPUWidgets::CoverageBatch& batch)
{
    dirty = false;
    ready = false;
    dropped = false;
    mesh.clear();

    if (path.isEmpty() || scale <= 0.f)
        return;

    // Tried first, and it takes no atlas slot at all when it works - which is
    // the whole point of it, the atlas being the thing a large shape exhausts.
    if (backing != Backing::Mask && buildMesh(scale))
        return;

    rasterizer.setScale(scale);
    rasterizer.setPath(path, fillRule);

    if (rasterizer.isEmpty())
        return;

    // Measured first, then placed: the atlas needs the size before it can say
    // where, and the kernel needs the where before it can be dispatched.
    auto width = rasterizer.getCoverageWidth();
    auto height = rasterizer.getCoverageHeight();

    // The room already reserved is kept whenever the new mask fits it. A knob
    // turning re-rasterizes every frame and its arc only ever shrinks, so the
    // steady state allocates nothing at all.
    if (!placed || width > slot.width || height > slot.height)
    {
        slot = atlas.allocate(width, height);
        placed = slot.width > 0;

        // Nowhere to put it: the atlas is at its ceiling. The shape draws as
        // nothing until the atlas is rebuilt -- which invalidates every shape,
        // so this one asks again then -- and says so meanwhile, that being the
        // only sign anything is missing.
        if (!placed)
        {
            dropped = true;
            return;
        }
    }

    rasterizer.setTarget(atlas.getTexture(), slot.x, slot.y);
    batch.add(rasterizer);

    maskUV = atlas.uvFor(slot.x, slot.y, width, height);
    bounds = rasterizer.getCoveredBounds();
    ready = true;
}
} // namespace eacp::UI
