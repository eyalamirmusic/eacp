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

// How many walks without a grow a shrink waits for, by default
// (CoverageAtlas::defaultQuietWalksToShrink). Counted in walks rather than
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
constexpr auto quietWalksDoublingCap = 16;

constexpr auto shelfRounding = 8;

int shelfHeightFor(int height)
{
    return (height + shelfRounding - 1) / shelfRounding * shelfRounding;
}

bool shelfSuits(int shelfHeight, int height)
{
    return height <= shelfHeight
           && shelfHeight - height < std::max(shelfRounding, height / 4);
}

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
{
    setQuietWalksToShrink(defaultQuietWalksToShrink);
    resizeTo(initialSize);
}

void CoverageAtlas::setQuietWalksToShrink(int walks)
{
    quietWalksNeeded = std::max(1, walks);
    maximumQuietWalks = quietWalksNeeded * quietWalksDoublingCap;
}

int CoverageAtlas::getWidth() const
{
    return texture->width();
}

int CoverageAtlas::getHeight() const
{
    return texture->height();
}

void CoverageAtlas::repackAt(int size)
{
    if (size == texture->width())
        reset();
    else
        resizeTo(size);

    ++atlasGeneration;
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
    shelves.clear();
    shelves.add({0, opaqueSize, opaqueSize, true});
    usedTexels = opaqueSize * opaqueSize;
    releasedTexels = 0;
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
    return shelves.size() == 1 && shelves[0].empty;
}

CoverageAtlas::Shelf* CoverageAtlas::findShelf(int width, int height)
{
    auto size = texture->width();
    auto* best = (Shelf*) nullptr;

    for (auto& shelf: shelves)
        if (shelf.x + width <= size && shelfSuits(shelf.height, height)
            && (best == nullptr || shelf.height < best->height))
            best = &shelf;

    if (best != nullptr)
        return best;

    auto& last = shelves.back();

    if (last.empty && last.x + width <= size && last.y + height <= size)
    {
        last.height =
            std::max(last.height, std::min(shelfHeightFor(height), size - last.y));
        return &last;
    }

    auto below = last.y + last.height;

    if (width <= size && below + height <= size)
    {
        shelves.add(
            {below, std::min(shelfHeightFor(height), size - below), 0, true});
        return &shelves.back();
    }

    for (auto& shelf: shelves)
        if (shelf.x + width <= size && height <= shelf.height
            && (best == nullptr || shelf.height < best->height))
            best = &shelf;

    return best;
}

std::optional<CoverageAtlas::Slot> CoverageAtlas::placeOnShelf(int width, int height)
{
    auto size = texture->width();

    if (width > size || height > size)
        return {};

    auto* shelf = findShelf(width, height);

    if (shelf == nullptr)
        return {};

    auto slot = Slot {};
    slot.x = shelf->x;
    slot.y = shelf->y;
    slot.width = width;
    slot.height = height;
    slot.uv = uvFor(slot.x, slot.y, width, height);

    shelf->x += width;
    shelf->empty = false;
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

    if (shouldCompactFor(width, height))
    {
        repackAt(texture->width());
        moved = true;
        compactedThisWalk = true;
        return true;
    }

    auto size = texture->width();

    if (size >= maximumSize)
        return false;

    auto grown = size * 2;

    while (grown < maximumSize && !fitsEmptyShelf(width, height, grown))
        grown *= 2;

    repackAt(grown);
    moved = true;

    // A grow inside the window after a shrink is the shrink having been
    // wrong, so the next one waits twice as long.
    if (walksSinceShrink < quietWalksNeeded)
        quietWalksNeeded = std::min(quietWalksNeeded * 2, maximumQuietWalks);

    walksSinceGrow = 0;
    return true;
}

bool CoverageAtlas::shouldCompactFor(int width, int height) const
{
    auto size = texture->width();

    if (isShelfEmpty() || compactionReplayed || !fitsEmptyShelf(width, height, size))
        return false;

    if (size >= maximumSize)
        return true;

    auto live = usedTexels - releasedTexels + (std::int64_t) width * height;

    return live * 2 <= (std::int64_t) size * size;
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
    compactedThisWalk = false;
    compactionReplayed = false;

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

    repackAt(target);

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
    compactionReplayed = compactedThisWalk;
    reset();
}

void CoverageAtlas::release(const Slot& slot)
{
    releasedTexels = std::min(
        releasedTexels + (std::int64_t) slot.width * slot.height, usedTexels);
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
