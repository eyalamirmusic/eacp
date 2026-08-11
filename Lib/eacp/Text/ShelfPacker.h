#pragma once

#include <optional>
#include <vector>

namespace eacp::Text
{
struct PackedRect
{
    int x = 0;
    int y = 0;
};

// Packs same-ish-height rectangles into a square, left to right in rows. A
// shelf takes its height from the first rectangle on it, and a rectangle goes
// on the first shelf that fits rather than the tightest one.
class ShelfPacker
{
public:
    ShelfPacker(int width, int height, int padding = 1);

    // Nothing when it does not fit. The returned origin accounts for padding,
    // so neighbours never share a texel and sampling cannot bleed between them.
    std::optional<PackedRect> add(int width, int height);

    // Forgets every placement; the caller is responsible for the pixels.
    void clear();

    void reset(int width, int height);

    // Keeps every existing placement, shelves only extending right and down.
    // Shrinking is not supported and is ignored.
    void grow(int width, int height);

    int width() const { return atlasWidth; }
    int height() const { return atlasHeight; }

    // Fraction of the atlas handed out, padding included and shelf gaps not.
    float occupancy() const;

private:
    struct Shelf
    {
        int y = 0;
        int height = 0;
        int penX = 0;
    };

    // A shelf taller than the glyph by more than this is passed over, wasting
    // the difference for the whole row.
    static constexpr int heightSlack = 2;

    int atlasWidth = 0;
    int atlasHeight = 0;
    int padding = 1;
    int nextShelfY = 0;
    long long usedArea = 0;

    std::vector<Shelf> shelves;
};
} // namespace eacp::Text
