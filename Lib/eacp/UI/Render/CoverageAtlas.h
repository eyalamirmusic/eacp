#pragma once

#include "../Common.h"

#include <eacp/GPUWidgets/GPUWidgets.h>

#include <cstdint>

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
//
// A move is only answerable while the caller can still start its walk again, so
// there is a pass on which it must not happen -- see setRelocationAllowed. On
// that pass an allocation that would move something is refused instead, and the
// refusals are counted, because an interface at the ceiling losing shapes with
// nothing said is the one failure here that is silent.
class CoverageAtlas
{
public:
    // A rect of the atlas, and the uv rect that samples it. An allocation that
    // could not be made yields a zero-width slot, which is what the caller tests
    // for -- the shape it was for draws as nothing rather than through texels
    // somebody else owns.
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
    // there is nowhere it can go: a mask larger than the largest atlas, or any
    // mask that needs room while relocation is not allowed.
    Slot allocate(int width, int height);

    // Whether an allocation may grow or compact to make room. Both move every
    // slot already handed out, so a caller can only survive one while it can
    // still start its whole walk again -- on the pass whose layout is the one
    // being drawn through, it cannot, and this is how it says so. An allocation
    // that would have moved something is refused and counted instead.
    void setRelocationAllowed(bool allowed) { relocationAllowed = allowed; }

    // Forgets every allocation, so the next one starts from an empty shelf.
    // Only sound when nothing holds a slot any more -- the caller has just
    // invalidated every shape it is about to rasterize again. Without it, a
    // second walk allocates beside what the first one placed and abandoned,
    // and an atlas the tree fits in is one it needs twice over.
    void forgetAllocations() { reset(); }

    // The uv rect for part of a slot, so a mask that came out smaller than the
    // room reserved for it can stay where it is instead of moving.
    Rect uvFor(int x, int y, int width, int height) const;

    // True when the last allocate() grew or compacted, and every slot handed
    // out before it is now wrong. Cleared by the reader.
    bool takeMovedFlag();

    // Masks there was no room for since the count was last cleared. Zero on any
    // interface that fits, and the only outward sign of the ceiling: each one
    // is a shape that will draw as nothing.
    int getDroppedCount() const { return dropped; }
    void clearDroppedCount() { dropped = 0; }

    // How much of the atlas is spoken for, counting the room reserved rather
    // than the coverage written into it, since it is the reservation that runs
    // out. The shelf never gives space back, so this is what approaching the
    // ceiling looks like from far enough away to do something about it.
    float getFillFraction() const;

    int getWidth() const;
    int getHeight() const;

private:
    // The two halves of an allocation: where it goes on the shelf as it stands,
    // and what can be done when the answer is nowhere. Separated because that is
    // what bounds the work -- one of each, in that order, and the second failure
    // is final.
    std::optional<Slot> placeOnShelf(int width, int height);
    bool makeRoomFor(int width, int height);

    bool isShelfEmpty() const;

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

    // Texels handed out, including the opaque corner, which is reserved in the
    // same sense even though no allocation produced it.
    std::int64_t usedTexels = 0;

    bool relocationAllowed = true;
    bool moved = false;
    int dropped = 0;
};
} // namespace eacp::UI
