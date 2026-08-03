#pragma once

#include "../Common.h"

namespace eacp::UI
{
// A second coverage mask everything drawn under it is multiplied by.
//
// A shape here is already a colour times a mask -- that is what the coverage
// atlas is -- so clipping to a vector region is one more multiply rather than a
// stencil, a second pass or a render target. The clip's own coverage is
// rasterized into the same atlas by the same kernel, and the fragment reads it
// at the position it is being drawn at.
//
// It is renderer *state* and not a per-shape field, which is the trade to know
// about: a run of shapes under one clip costs nothing per shape and a change of
// clip costs a batch break, exactly as the scissor rect does. Set once per
// clipped group rather than once per shape.
//
// Note what it cannot do. Two of these do not intersect: a fragment reads one
// mask, so a clip inside a clip has to be composed into a single region before
// it arrives -- see Graphics::reduceClipToShape for what happens instead. And
// glyphs are drawn by a renderer that samples no atlas, so text is cut by the
// scissor rect and not by this.
struct ClipMask
{
    // Where the mask lands, in the space the batch draws in. A fragment's place
    // inside this rect is what indexes the coverage, and a fragment outside it
    // is outside the clip.
    Rect region;

    // The atlas rect that coverage was rasterized into.
    Rect uv;

    bool isEmpty() const { return region.w <= 0.f || region.h <= 0.f; }
};

inline bool sameClipMask(const ClipMask& a, const ClipMask& b)
{
    return sameRect(a.region, b.region) && sameRect(a.uv, b.uv);
}
} // namespace eacp::UI
