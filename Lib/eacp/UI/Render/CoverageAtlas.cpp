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

CoverageAtlas::Slot CoverageAtlas::allocate(int width, int height)
{
    auto empty = Slot {};
    empty.uv = getOpaqueUV();

    if (width <= 0 || height <= 0)
        return empty;

    auto size = texture->width();

    // Wider than the atlas, or past its last row: make room, then place from a
    // clean shelf. Growing is the first answer and compacting the second, since
    // one keeps everything that was already placed and the other does not - but
    // both move every slot, and both say so.
    while (width > size || height > size || cursorY + height > size)
    {
        if (size < maximumSize)
        {
            size *= 2;
            resizeTo(size);
        }
        else if (cursorY > 0)
        {
            reset();
        }
        else
        {
            // A single mask larger than the largest atlas. Nothing to do but
            // refuse it, and the shape draws as nothing rather than as
            // somebody else's coverage.
            return empty;
        }

        moved = true;
    }

    if (cursorX + width > size)
    {
        cursorX = 0;
        cursorY += rowHeight;
        rowHeight = 0;

        // Starting a row may itself run off the bottom, which is the case the
        // loop above handles - so go back through it rather than placing into a
        // row that is not there.
        if (cursorY + height > size)
            return allocate(width, height);
    }

    auto slot = Slot {};
    slot.x = cursorX;
    slot.y = cursorY;
    slot.width = width;
    slot.height = height;
    slot.uv = uvFor(slot.x, slot.y, width, height);

    cursorX += width;
    rowHeight = std::max(rowHeight, height);

    return slot;
}
} // namespace eacp::UI
