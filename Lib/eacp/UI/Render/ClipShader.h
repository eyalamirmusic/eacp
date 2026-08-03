#pragma once

#include "ClipMask.h"

#include <eacp/GPU/GPU.h>

namespace eacp::UI
{
// How much of the clip reaches a fragment, written once for both shape
// renderers -- the same arrangement GradientShader is, and for the same reason:
// a quad and a triangle differ in how a fragment is reached and not at all in
// what it then does.
//
// `region` is the clip's rect as (x, y, 1/width, 1/height) and `mask` its atlas
// rect as (u, v, width, height). A fragment's place inside the region indexes
// the coverage; one outside it is outside the clip and gets nothing.
//
// The unclipped case is the same arithmetic rather than a branch: a region of
// zero size maps every fragment to the middle of the rect, and pointing `mask`
// at the atlas's opaque texel then multiplies by one. So a shape with no clip
// pays a fetch of a texel that is in cache for the whole frame -- which is the
// trade the mask fetch beside it already makes, against a second pipeline and a
// batch break between clipped and unclipped shapes.
inline GPU::Float clipCoverage(const GPU::Float2& position,
                               const GPU::Float4& region,
                               const GPU::Float4& mask,
                               GPU::Uniform<GPU::Texture2D>& atlas)
{
    auto place = (position - region.xy()) * region.zw();

    // Measured from the middle rather than tested against both ends, so the
    // unclipped case -- which lands exactly on zero -- is inside by the same
    // comparison that keeps a real clip's own edges.
    auto fromCentre = abs(place - 0.5f);
    auto inside = step(fromCentre.x(), 0.5f) * step(fromCentre.y(), 0.5f);

    return inside * sample(atlas, mask.xy() + place * mask.zw()).x();
}

// The two uniforms the function above reads, filled in from a clip and the uv
// of the atlas texel that stands for no clip at all.
inline void packClipMask(const ClipMask& clip,
                         const Rect& opaqueUV,
                         Array<float, 4>& region,
                         Array<float, 4>& mask)
{
    using Slot = Array<float, 4>;

    if (clip.isEmpty())
    {
        region = Slot {0.f, 0.f, 0.f, 0.f};
        mask = Slot {opaqueUV.x, opaqueUV.y, opaqueUV.w, opaqueUV.h};
        return;
    }

    region = Slot {
        clip.region.x, clip.region.y, 1.f / clip.region.w, 1.f / clip.region.h};

    mask = Slot {clip.uv.x, clip.uv.y, clip.uv.w, clip.uv.h};
}
} // namespace eacp::UI
