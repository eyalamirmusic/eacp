#include "CoverageAtlas.h"

#include <algorithm>

namespace eacp::UI
{
namespace
{
// The opaque corner every unmasked shape samples. Four texels rather than one
// so a filtered read could not reach past it into a neighbour's coverage.
constexpr auto opaqueSize = 4;

constexpr auto initialSize = 64;
constexpr auto maximumSize = 4096;

// Whether a mask fits an atlas of `size` holding nothing but the opaque corner:
// beside it in the first row, or on the row below. This is the one refusal that
// no amount of moving things can answer.
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

    texture->update({0.f, 0.f, (float) opaqueSize, (float) opaqueSize}, white);
}

void CoverageAtlas::reset()
{
    // The first shelf starts beside the opaque corner, which is never allocated.
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
    // Zero-sized, so every fragment reads the same texel - the corner's centre,
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

    // Next row, below the tallest thing on this one. Worked out before anything
    // is committed, so a row running off the bottom leaves the shelf as it was.
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
    // Every way of making room moves every slot already handed out.
    if (!relocationAllowed)
        return false;

    // No atlas this implementation can build would hold it.
    if (!fitsEmptyShelf(width, height, maximumSize))
        return false;

    auto size = texture->width();

    // Straight to a size that holds this mask rather than one doubling per
    // attempt, so the caller's next placement cannot fail for want of room.
    if (size < maximumSize)
    {
        auto grown = size * 2;

        while (grown < maximumSize && !fitsEmptyShelf(width, height, grown))
            grown *= 2;

        resizeTo(grown);
        moved = true;
        ++atlasGeneration;
        return true;
    }

    // At maximum size, so compacting is all that is left.
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

    // Two attempts at most, never recursive: a mask as large as the atlas can
    // make room and still not fit, and would ask for room again forever.
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
