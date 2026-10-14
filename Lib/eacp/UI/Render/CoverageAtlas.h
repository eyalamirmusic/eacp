#pragma once

#include "../Common.h"

#include <eacp/GPUWidgets/GPUWidgets.h>

#include <cstdint>
#include <limits>

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
// Allocation is a set of shelves: rows filled left to right, each holding
// masks of about its own height, a new one started below the last. There is no
// free list, because there is nothing sensible to do with a hole a widget-sized
// mask leaves behind. Instead a shape keeps the slot it was given for as long
// as its mask still fits in it - which for a knob being dragged is always,
// since the arc only shrinks - and when the shelves do run out the atlas
// compacts if little of what it holds is still in use, and grows otherwise.
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

    const GPU::Texture& getTexture() const;

    // What a shape with no mask samples: a uv rect inside the opaque texel.
    Rect getOpaqueUV() const;

    // Room for a mask of this many device pixels, or a zero-width slot when
    // there is nowhere it can go: a mask larger than the largest atlas, or any
    // mask that needs room while relocation is not allowed.
    Slot allocate(int width, int height);
    void release(const Slot& slot);

    // Whether an allocation may grow or compact to make room. Both move every
    // slot already handed out, so a caller can only survive one while it can
    // still start its whole walk again -- on the pass whose layout is the one
    // being drawn through, it cannot, and this is how it says so. An allocation
    // that would have moved something is refused and counted instead.
    void setRelocationAllowed(bool allowed);

    // Forgets every allocation, so the next one starts from an empty shelf.
    // Only sound when nothing holds a slot any more -- the caller has just
    // invalidated every shape it is about to rasterize again. Without it, a
    // second walk allocates beside what the first one placed and abandoned,
    // and an atlas the tree fits in is one it needs twice over.
    void forgetAllocations();

    // The uv rect for part of a slot, so a mask that came out smaller than the
    // room reserved for it can stay where it is instead of moving.
    Rect uvFor(int x, int y, int width, int height) const;

    // True when the last allocate() grew or compacted, and every slot handed
    // out before it is now wrong. Cleared by the reader.
    bool takeMovedFlag();

    // Bumped whenever every uv already handed out stops meaning what it did.
    // The flag above is for the caller that reads it once and re-rasterizes; this
    // is for the one that *recorded* a uv rather than asking for one each frame,
    // and has to notice at some later point that what it kept is stale.
    std::uint32_t generation() const;

    // Masks there was no room for since the count was last cleared. Zero on any
    // interface that fits, and the only outward sign of the ceiling: each one
    // is a shape that will draw as nothing.
    int getDroppedCount() const;
    void clearDroppedCount();

    // How much of the atlas is spoken for, counting the room reserved rather
    // than the coverage written into it, since it is the reservation that runs
    // out. The shelf never gives space back, so this is what approaching the
    // ceiling looks like from far enough away to do something about it.
    float getFillFraction() const;

    int getWidth() const;
    int getHeight() const;

    // The format the texture was created in: R8Unorm where the device can
    // store to it, RGBA8Unorm where it cannot. See GPUWidgets::coverageFormat.
    GPU::TextureFormat getFormat() const;

    // Gives back room the shelf has stopped needing. The atlas only grows while
    // it allocates, so an interface that once needed the ceiling -- every page
    // rasterized at startup, say, or a compaction that found most of it
    // abandoned -- would otherwise keep the whole texture for good.
    //
    // Shrinks to the smallest size that holds the largest mask placed since the
    // shelf was last emptied with what has been reserved filling an eighth of it
    // at most, and only when that is smaller than the atlas is now. Like a grow,
    // it moves every slot, so it is only for a caller that is about to
    // rasterize everything again (the generation says so); unlike one, it does
    // not raise the moved flag, since nothing has been placed in the new atlas
    // yet. Refused while relocation is not allowed.
    //
    // Meant to be asked once a walk, and that is what it counts: it shrinks
    // only once that many walks have passed without the atlas growing (see
    // getQuietWalksToShrink), so a slot churning under an animation cannot turn
    // it into a resize every few frames.
    //
    // The reservation counts every slot placed since the shelf was last
    // emptied -- abandoned ones, and those of shapes that have since been
    // hidden or removed -- so hiding a page gives nothing back by itself. The
    // shelf is emptied only by a grow or a compaction, and it is after one of
    // those that this has something to return.
    bool shrinkToFit();

    // The walks without a grow a shrink currently waits for. It starts at
    // defaultQuietWalksToShrink and doubles, up to sixteen times that, each
    // time a shrink is outgrown inside its window.
    int getQuietWalksToShrink() const;

    // Restarts that window at this many walks, with its cap at sixteen times
    // it. For a test that has to watch a shrink happen without drawing two
    // seconds of frames to get there.
    void setQuietWalksToShrink(int walks);

    static constexpr int defaultQuietWalksToShrink = 120;

private:
    struct Shelf
    {
        int y = 0;
        int height = 0;
        int x = 0;
        bool empty = true;
    };

    // The two halves of an allocation: where it goes on the shelf as it stands,
    // and what can be done when the answer is nowhere. Separated because that is
    // what bounds the work -- one of each, in that order, and the second failure
    // is final.
    std::optional<Slot> placeOnShelf(int width, int height);
    Shelf* findShelf(int width, int height);
    bool makeRoomFor(int width, int height);
    bool shouldCompactFor(int width, int height) const;

    bool isShelfEmpty() const;

    void repackAt(int size);
    void resizeTo(int size);
    void seedOpaqueTexel();

    // Forgets every allocation. The texture and its opaque texel survive.
    void reset();

    std::optional<GPU::Texture> texture;

    Vector<Shelf> shelves;

    // Texels handed out, including the opaque corner, which is reserved in the
    // same sense even though no allocation produced it.
    std::int64_t usedTexels = 0;
    std::int64_t releasedTexels = 0;

    // The largest mask placed since the shelf was last emptied, which a smaller
    // atlas still has to hold.
    int widestSlot = 0;
    int tallestSlot = 0;

    GPU::TextureFormat format = GPU::TextureFormat::RGBA8Unorm;

    // The hysteresis: walks since the atlas last grew and since it last shrank,
    // and how many quiet walks a shrink waits for. Since-shrink starts out of
    // any window, so the first grows of a new atlas -- the tree arriving -- do
    // not count against a shrink that never happened.
    int walksSinceGrow = 0;
    int walksSinceShrink = std::numeric_limits<int>::max() / 2;
    int quietWalksNeeded = 0;
    int maximumQuietWalks = 0;

    bool compactedThisWalk = false;
    bool compactionReplayed = false;

    bool relocationAllowed = true;
    bool moved = false;
    int dropped = 0;

    std::uint32_t atlasGeneration = 0;
};
} // namespace eacp::UI
