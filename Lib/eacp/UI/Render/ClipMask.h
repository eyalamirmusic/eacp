#pragma once

#include "../Common.h"

namespace eacp::UI
{
// A second coverage mask everything drawn under it is multiplied by. Renderer
// state, so a change costs a batch break; two of these cannot intersect, and
// text is cut by the scissor rect instead.
struct ClipMask
{
    // Where the mask lands, in the space the batch draws in; a fragment's place
    // inside it indexes the coverage.
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
