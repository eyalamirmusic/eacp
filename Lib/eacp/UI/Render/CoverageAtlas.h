#pragma once

#include "../Common.h"

#include <eacp/GPUWidgets/GPUWidgets.h>

namespace eacp::UI
{
// One texture every path in the interface rasterizes into, so that drawing a
// path costs a quad rather than a texture bind and a batch break.
//
// The alternative - a coverage texture per path - is what makes vector shapes
// expensive in a batching renderer: each one is a different resource, so each
// one ends the run of quads before it. Here the kernel writes into a rect of
// this texture (PathRasterizer::setTarget), the shape carries the matching uv
// sub-rect, and a hundred paths go out in the same instanced draw as the
// hundred rounded rectangles around them.
//
// Texel (0, 0) is deliberately opaque, and the uv of it is what every *unmasked*
// shape samples. That is what keeps masked and unmasked shapes on one pipeline:
// the fragment stage always multiplies by the atlas, and for a plain rectangle
// it multiplies by one. It costs a fetch of a texel that is in the cache for the
// whole frame, against the batch break a second pipeline would cost.
//
// Allocation is a shelf: rows filled left to right, a new row started below the
// tallest so far. There is no free list, because there is nothing sensible to
// do with a hole a widget-sized mask leaves behind. Instead a shape keeps the
// slot it was given for as long as its mask still fits in it - which for a knob
// being dragged is always, since the arc only shrinks - and when the shelf does
// run out the atlas grows, or compacts if it is already as large as it goes.
//
// Either of those moves every slot already handed out, which hasMoved() reports
// so the caller can re-rasterize the lot. That is the expensive frame, and it is
// rare: the steady state is that nothing allocates at all.
class CoverageAtlas
{
public:
    // A rect of the atlas, and the uv rect that samples it. An allocation that
    // did not fit yields the opaque slot, so a shape drawn through it comes out
    // as its own unmasked self rather than as nothing.
    struct Slot
    {
        int x = 0;
        int y = 0;
        int width = 0;
        int height = 0;

        Rect uv;
    };

    CoverageAtlas();

    const GPU::Texture& getTexture() const { return *texture; }

    // What a shape with no mask samples: a uv rect inside the opaque texel.
    Rect getOpaqueUV() const;

    // Room for a mask of this many device pixels, or a zero-width slot when
    // even a compacted atlas at its largest cannot hold it.
    Slot allocate(int width, int height);

    // The uv rect for part of a slot, so a mask that came out smaller than the
    // room reserved for it can stay where it is instead of moving.
    Rect uvFor(int x, int y, int width, int height) const;

    // True when the last allocate() grew or compacted, and every slot handed
    // out before it is now wrong. Cleared by the reader.
    bool takeMovedFlag();

    int getWidth() const;
    int getHeight() const;

private:
    void resizeTo(int size);
    void seedOpaqueTexel();

    // Forgets every allocation. The texture and its opaque texel survive.
    void reset();

    std::optional<GPU::Texture> texture;

    // The shelf cursor: where the next slot goes, and how far down the next row
    // starts.
    int cursorX = 0;
    int cursorY = 0;
    int rowHeight = 0;

    bool moved = false;
};
} // namespace eacp::UI
