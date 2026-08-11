#pragma once

#include "ClipMask.h"

#include <eacp/GPU/GPU.h>

namespace eacp::UI
{
// How much of the clip reaches a fragment, shared by both shape renderers.
// `region` is the clip rect as (x, y, 1/width, 1/height), `mask` its atlas rect
// as (u, v, width, height). Unclipped is a zero-size region, not a branch.
inline GPU::Float clipCoverage(const GPU::Float2& position,
                               const GPU::Float4& region,
                               const GPU::Float4& mask,
                               GPU::Uniform<GPU::Texture2D>& atlas)
{
    auto place = (position - region.xy()) * region.zw();

    // From the middle rather than against both ends, so the unclipped case
    // (exactly zero) is inside by the same comparison.
    auto fromCentre = abs(place - 0.5f);
    auto inside = step(fromCentre.x(), 0.5f) * step(fromCentre.y(), 0.5f);

    return inside * sample(atlas, mask.xy() + place * mask.zw()).x();
}

// `opaqueUV` is the atlas texel that stands for no clip at all.
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
