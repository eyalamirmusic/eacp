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

// Packs same-ish-height rectangles into a square, left to right in rows.
//
// Shelf packing, the strategy Alacritty's atlas uses. Glyphs from one face at
// one size vary a lot in width but hardly at all in height, and a shelf packer
// is close to optimal for exactly that shape while staying simple enough to
// verify. (Ghostty uses a skyline packer, which handles wildly varying heights
// better; that is not the workload here.)
//
// A shelf is a row with a fixed height, set by the first glyph placed on it. A
// glyph goes on the first shelf that is tall enough and does not waste too much
// height, otherwise a new shelf opens below. Deliberately not the *best* fit —
// scanning for the tightest shelf costs more than the space it recovers, since
// heights are near-uniform to begin with.
class ShelfPacker
{
public:
    ShelfPacker(int width, int height, int padding = 1);

    // Reserves width x height, or nothing when it does not fit. The returned
    // origin already accounts for padding, so neighbouring glyphs never share a
    // texel and linear sampling cannot bleed one into the next.
    std::optional<PackedRect> add(int width, int height);

    // Forgets every placement; the caller is responsible for the pixels.
    void clear();

    // Same, at a new size — used when the atlas grows.
    void reset(int width, int height);

    // Enlarges the packing area, keeping every existing placement.
    //
    // Safe because a shelf only ever extends right and down from a fixed
    // origin: nothing already handed out moves when the bounds get bigger, so
    // the atlas can double without re-rasterizing a single glyph. Shrinking is
    // not supported and is ignored.
    void grow(int width, int height);

    int width() const { return atlasWidth; }
    int height() const { return atlasHeight; }

    // Fraction of the atlas handed out, padding included. Only meaningful as a
    // rough occupancy signal, since shelves leave gaps this does not model.
    float occupancy() const;

private:
    struct Shelf
    {
        int y = 0;
        int height = 0;
        int penX = 0;
    };

    // A shelf taller than the glyph by more than this is passed over: dropping a
    // short glyph onto a tall shelf wastes the difference for the whole row.
    static constexpr int heightSlack = 2;

    int atlasWidth = 0;
    int atlasHeight = 0;
    int padding = 1;
    int nextShelfY = 0;
    long long usedArea = 0;

    std::vector<Shelf> shelves;
};
} // namespace eacp::Text
