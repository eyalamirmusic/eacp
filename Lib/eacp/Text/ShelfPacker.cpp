#include "ShelfPacker.h"

#include <algorithm>

namespace eacp::Text
{
ShelfPacker::ShelfPacker(int width, int height, int paddingToUse)
    : atlasWidth(width)
    , atlasHeight(height)
    , padding(std::max(paddingToUse, 0))
{
    clear();
}

void ShelfPacker::clear()
{
    shelves.clear();
    nextShelfY = padding;
    usedArea = 0;
}

void ShelfPacker::reset(int width, int height)
{
    atlasWidth = width;
    atlasHeight = height;
    clear();
}

void ShelfPacker::grow(int width, int height)
{
    // Existing placements keep their coordinates, so the shelves carry over
    // untouched and no glyph needs re-rasterizing.
    atlasWidth = std::max(atlasWidth, width);
    atlasHeight = std::max(atlasHeight, height);
}

std::optional<PackedRect> ShelfPacker::add(int width, int height)
{
    if (width <= 0 || height <= 0)
        return std::nullopt;

    // Padding is reserved on the right and below each placement, so a glyph
    // needs its own size plus one gap to fit.
    const auto neededWidth = width + padding;
    const auto neededHeight = height + padding;

    if (width + padding * 2 > atlasWidth || height + padding * 2 > atlasHeight)
        return std::nullopt;

    for (auto& shelf: shelves)
    {
        if (shelf.height < height || shelf.height > height + heightSlack)
            continue;

        if (shelf.penX + neededWidth > atlasWidth)
            continue;

        const auto placed = PackedRect {shelf.penX, shelf.y};
        shelf.penX += neededWidth;
        usedArea += static_cast<long long>(neededWidth) * neededHeight;

        return placed;
    }

    if (nextShelfY + neededHeight > atlasHeight)
        return std::nullopt;

    auto shelf = Shelf {nextShelfY, height, padding};
    const auto placed = PackedRect {shelf.penX, shelf.y};

    shelf.penX += neededWidth;
    nextShelfY += neededHeight;
    usedArea += static_cast<long long>(neededWidth) * neededHeight;

    shelves.push_back(shelf);
    return placed;
}

float ShelfPacker::occupancy() const
{
    const auto total = static_cast<long long>(atlasWidth) * atlasHeight;

    if (total <= 0)
        return 0.f;

    return static_cast<float>(usedArea) / static_cast<float>(total);
}
} // namespace eacp::Text
