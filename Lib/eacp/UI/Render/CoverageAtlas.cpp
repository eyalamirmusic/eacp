#include "CoverageAtlas.h"

#include <eacp/GPUWidgets/Path/CoverageKernel.h>

#include <algorithm>

namespace eacp::UI
{
namespace
{
// The opaque corner every unmasked shape samples. Four texels rather than one
// so a filtered read - which this does not do today - could still not reach
// past it into a neighbour's coverage.
constexpr auto opaqueSize = 4;

// Starts holding nothing but the opaque corner, and doubles from there, so an
// interface with no vector shapes in it carries kilobytes rather than
// megabytes. The ceiling is where a UI atlas stops being the right structure:
// past it the answer is to stop rasterizing whole paths and start binning them.
constexpr auto initialSize = 64;
constexpr auto maximumSize = 4096;

// How many walks without a grow a shrink waits for. Counted in walks rather than
// in time, because a walk is what a resize costs -- a whole tree rasterized
// again -- and an interface that is not drawing is not paying for the room it
// holds any more than it would pay to give it back. 120 is two seconds of a
// 60 Hz animation: long enough that a slot churning under one (a path being
// dragged, a menu rebuilt per open) has shown whether it needs the room, short
// enough that an interface done with it gets it back while it is still drawing.
//
// Doubled each time a shrink is outgrown inside its window, up to the cap, and
// not latched: an interface that keeps needing the room pays for one wasted
// resize per doubling, and one that grew back by coincidence -- a page switch
// on the frame after -- has only made the next shrink wait longer.
constexpr auto quietWalksToShrink = 120;
constexpr auto maximumQuietWalks = quietWalksToShrink * 16;

// Where a mask lands on an empty shelf: beside the opaque corner if the room
// left in the first row is wide enough, and on the row below it otherwise. A
// mask that fits neither does not fit an *empty* atlas of this size, which is
// the one refusal no amount of moving things can answer.
//
// The second case is why a mask exactly as wide and as tall as the largest atlas
// has to be refused rather than made room for: it needs the first row, and the
// first row is four texels short of it for as long as the opaque corner is
// there.
bool fitsEmptyShelf(int width, int height, int size)
{
    if (width <= size - opaqueSize && height <= size)
        return true;

    return width <= size && height <= size - opaqueSize;
}

GPU::TextureDescriptor describeAtlas(int size)
{
    auto descriptor = GPU::TextureDescriptor {};
    descriptor.width = size;
    descriptor.height = size;
    descriptor.format = GPUWidgets::coverageFormat();
    descriptor.computeWrite = true;
    return descriptor;
}
} // namespace

CoverageAtlas::CoverageAtlas()
    : quietWalksNeeded(quietWalksToShrink)
{
    resizeTo(initialSize);
}

int CoverageAtlas::getWidth() const
{
    return texture->width();
}

int CoverageAtlas::getHeight() const
{
    return texture->height();
}

void CoverageAtlas::resizeTo(int size)
{
    auto descriptor = describeAtlas(size);
    format = descriptor.format;
    texture.emplace(GPU::Device::shared(), descriptor, nullptr);

    seedOpaqueTexel();
    reset();
}

void CoverageAtlas::seedOpaqueTexel()
{
    // Sized for the widest format the atlas can be, and uploaded at the one it
    // is - R8 takes the first quarter of it.
    std::uint8_t white[opaqueSize * opaqueSize * 4];
    std::fill(std::begin(white), std::end(white), (std::uint8_t) 255);

    // A kernel-written texture still takes a CPU upload, which is the only way
    // to put a constant in one without dispatching a kernel to write it.
    texture->update({0.f, 0.f, (float) opaqueSize, (float) opaqueSize}, white);
}

void CoverageAtlas::reset()
{
    // The opaque corner is part of the texture rather than an allocation, so the
    // first shelf starts beside it and no slot can ever be handed it by mistake.
    cursorX = opaqueSize;
    cursorY = 0;
    rowHeight = opaqueSize;
    usedTexels = opaqueSize * opaqueSize;
    widestSlot = 0;
    tallestSlot = 0;
}

float CoverageAtlas::getFillFraction() const
{
    auto extent = (std::int64_t) texture->width();

    return (float) ((double) usedTexels / (double) (extent * extent));
}

Rect CoverageAtlas::getOpaqueUV() const
{
    // A zero-sized uv rect, so every fragment of an unmasked shape reads the
    // same texel however large the shape is - the centre of the opaque corner,
    // far enough from its edge that no rounding lands outside.
    auto centre = (float) (opaqueSize / 2) + 0.5f;
    auto size = (float) texture->width();

    return {centre / size, centre / size, 0.f, 0.f};
}

bool CoverageAtlas::takeMovedFlag()
{
    auto result = moved;
    moved = false;
    return result;
}

Rect CoverageAtlas::uvFor(int x, int y, int width, int height) const
{
    auto extent = (float) texture->width();

    return {(float) x / extent,
            (float) y / extent,
            (float) width / extent,
            (float) height / extent};
}

bool CoverageAtlas::isShelfEmpty() const
{
    return cursorY == 0 && cursorX == opaqueSize;
}

std::optional<CoverageAtlas::Slot> CoverageAtlas::placeOnShelf(int width, int height)
{
    auto size = texture->width();

    if (width > size || height > size)
        return {};

    auto x = cursorX;
    auto y = cursorY;
    auto row = rowHeight;

    // The current row is out of width, so the next one starts below the tallest
    // thing on this one. Worked out before anything is committed, because a row
    // that turns out to run off the bottom must leave the shelf as it was --
    // the caller is about to move the whole atlas, and it will start again from
    // an empty one anyway.
    if (x + width > size)
    {
        x = 0;
        y += row;
        row = 0;
    }

    if (y + height > size)
        return {};

    auto slot = Slot {};
    slot.x = x;
    slot.y = y;
    slot.width = width;
    slot.height = height;
    slot.uv = uvFor(x, y, width, height);

    cursorX = x + width;
    cursorY = y;
    rowHeight = std::max(row, height);
    usedTexels += (std::int64_t) width * height;
    widestSlot = std::max(widestSlot, width);
    tallestSlot = std::max(tallestSlot, height);

    return slot;
}

bool CoverageAtlas::makeRoomFor(int width, int height)
{
    // Every way of making room moves every slot already handed out, and on this
    // pass the caller cannot answer that -- so there is no room to be had. The
    // mask is refused, and the count is the only way anybody hears about it.
    if (!relocationAllowed)
        return false;

    // Nothing to try: no atlas this implementation can build would hold it.
    if (!fitsEmptyShelf(width, height, maximumSize))
        return false;

    auto size = texture->width();

    // Growing is the first answer while there is anywhere to grow to, being the
    // one that ends with more room than it started with -- an interface that
    // keeps needing a little more settles at a size instead of compacting every
    // frame. Straight to a size that holds this mask rather than one doubling
    // per attempt, so the caller's next placement cannot fail for want of room
    // that another doubling would have found.
    if (size < maximumSize)
    {
        auto grown = size * 2;

        while (grown < maximumSize && !fitsEmptyShelf(width, height, grown))
            grown *= 2;

        resizeTo(grown);
        moved = true;
        ++atlasGeneration;

        // A grow inside the window after a shrink is the shrink having been
        // wrong, so the next one waits twice as long.
        if (walksSinceShrink < quietWalksNeeded)
            quietWalksNeeded = std::min(quietWalksNeeded * 2, maximumQuietWalks);

        walksSinceGrow = 0;
        return true;
    }

    // As large as it goes, so compacting is what is left -- and only if there is
    // something to compact. An empty shelf at the largest size has already given
    // everything it has.
    if (isShelfEmpty())
        return false;

    reset();
    moved = true;
    ++atlasGeneration;
    return true;
}

CoverageAtlas::Slot CoverageAtlas::allocate(int width, int height)
{
    auto empty = Slot {};
    empty.uv = getOpaqueUV();

    if (width <= 0 || height <= 0)
        return empty;

    // Twice at most, and that is the shape rather than an optimisation: place it
    // on the shelf as it stands, and failing that make room once and place it on
    // a shelf that is empty. There is no third thing to try, so a second failure
    // is the ceiling -- which is what the count is for.
    //
    // Written as a recursion until it was asked for a mask as large as the atlas
    // itself: room was made, the mask still did not fit the first row, and
    // making room again is what the next call did. Not slow, not wrong, not
    // silent -- it never returned.
    if (auto slot = placeOnShelf(width, height))
        return *slot;

    if (makeRoomFor(width, height))
    {
        if (auto slot = placeOnShelf(width, height))
            return *slot;
    }

    ++dropped;
    return empty;
}

bool CoverageAtlas::shrinkToFit()
{
    walksSinceGrow = std::min(walksSinceGrow + 1, maximumQuietWalks);
    walksSinceShrink = std::min(walksSinceShrink + 1, maximumQuietWalks);

    if (!relocationAllowed || walksSinceGrow < quietWalksNeeded)
        return false;

    auto size = texture->width();
    auto target = initialSize;

    while (target < size
           && (!fitsEmptyShelf(widestSlot, tallestSlot, target)
               || usedTexels * 8 > (std::int64_t) target * target))
        target *= 2;

    if (target >= size)
        return false;

    resizeTo(target);
    ++atlasGeneration;

    // A resize as much as a grow is, so the next one waits its own window.
    walksSinceGrow = 0;
    walksSinceShrink = 0;
    return true;
}

int CoverageAtlas::getQuietWalksToShrink() const
{
    return quietWalksNeeded;
}

GPU::TextureFormat CoverageAtlas::getFormat() const
{
    return format;
}

const GPU::Texture& CoverageAtlas::getTexture() const
{
    return *texture;
}

void CoverageAtlas::setRelocationAllowed(bool allowed)
{
    relocationAllowed = allowed;
}

void CoverageAtlas::forgetAllocations()
{
    reset();
}

std::uint32_t CoverageAtlas::generation() const
{
    return atlasGeneration;
}

int CoverageAtlas::getDroppedCount() const
{
    return dropped;
}

void CoverageAtlas::clearDroppedCount()
{
    dropped = 0;
}
} // namespace eacp::UI
