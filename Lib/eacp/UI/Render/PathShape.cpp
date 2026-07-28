#include "PathShape.h"

#include "../Component/Component.h"

namespace eacp::UI
{
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

void PathShape::clear()
{
    path.clear();
    dirty = true;
    ready = false;
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

void PathShape::rasterize(CoverageAtlas& atlas, float scale, GPU::ComputePass& pass)
{
    dirty = false;
    ready = false;

    if (path.isEmpty() || scale <= 0.f)
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

        if (!placed)
            return;
    }

    rasterizer.setTarget(atlas.getTexture(), slot.x, slot.y);
    rasterizer.dispatch(pass);

    maskUV = atlas.uvFor(slot.x, slot.y, width, height);
    bounds = rasterizer.getCoveredBounds();
    ready = true;
}
} // namespace eacp::UI
