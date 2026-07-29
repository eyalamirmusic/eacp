#include "CoverageAtlas.h"

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
    descriptor.format = GPU::TextureFormat::RGBA8Unorm;
    descriptor.computeWrite = true;
    return descriptor;
}
} // namespace

CoverageAtlas::CoverageAtlas()
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
    texture.emplace(GPU::Device::shared(), describeAtlas(size), nullptr);

    seedOpaqueTexel();
    reset();
}

void CoverageAtlas::seedOpaqueTexel()
{
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
        return true;
    }

    // As large as it goes, so compacting is what is left -- and only if there is
    // something to compact. An empty shelf at the largest size has already given
    // everything it has.
    if (isShelfEmpty())
        return false;

    reset();
    moved = true;
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
} // namespace eacp::UI
