#include "Common.h"

// The shelf packer, which is pure arithmetic — no GPU, no fonts, no platform.
// That is deliberate: packing is where an atlas goes subtly wrong (overlaps
// that show as one glyph bleeding into another), and it is much easier to pin
// down here than by staring at a texture.

using namespace nano;
using namespace eacp::Text;

namespace
{
// Every placement must be inside the atlas and must not overlap any earlier
// one. Checked exhaustively rather than by spot-check, since an overlap is
// exactly the bug that renders as a corrupted glyph.
struct Placements
{
    struct Box
    {
        int x, y, w, h;
    };

    std::vector<Box> boxes;

    bool add(const PackedRect& at, int w, int h, int atlasSize)
    {
        if (at.x < 0 || at.y < 0 || at.x + w > atlasSize || at.y + h > atlasSize)
            return false;

        for (const auto& box: boxes)
        {
            const auto separated = at.x + w <= box.x || box.x + box.w <= at.x
                                   || at.y + h <= box.y || box.y + box.h <= at.y;

            if (!separated)
                return false;
        }

        boxes.push_back({at.x, at.y, w, h});
        return true;
    }
};
} // namespace

auto tPacksFirstRectAtPadding = test("ShelfPacker/firstRectSitsInsideThePadding") = []
{
    auto packer = ShelfPacker {64, 64, 1};

    const auto placed = packer.add(10, 10);

    check(placed.has_value());
    check(placed->x == 1);
    check(placed->y == 1);
};

// Same-height glyphs share a row, which is the whole point of shelf packing.
auto tSameHeightShareAShelf = test("ShelfPacker/sameHeightRectsShareAShelf") = []
{
    auto packer = ShelfPacker {128, 128, 1};

    const auto first = packer.add(10, 12);
    const auto second = packer.add(10, 12);

    check(first.has_value());
    check(second.has_value());
    check(first->y == second->y);
    check(second->x > first->x);
};

// Padding must actually separate them, or linear sampling pulls one glyph's
// coverage into its neighbour's edge.
auto tLeavesPaddingBetween = test("ShelfPacker/leavesPaddingBetweenRects") = []
{
    auto packer = ShelfPacker {128, 128, 1};

    const auto first = packer.add(10, 12);
    const auto second = packer.add(10, 12);

    check(second->x >= first->x + 10 + 1);
};

auto tOpensNewShelfWhenRowIsFull = test("ShelfPacker/opensANewShelfWhenTheRowFills") = []
{
    auto packer = ShelfPacker {64, 128, 1};

    const auto first = packer.add(30, 10);
    packer.add(30, 10);

    // The third no longer fits beside the first two.
    const auto third = packer.add(30, 10);

    check(third.has_value());
    check(third->y > first->y);
    check(third->x == 1);
};

// A short glyph must not be dropped onto a much taller shelf: the wasted height
// applies to the whole row, so it is cheaper to open a new one.
auto tSkipsShelvesThatWasteHeight = test("ShelfPacker/skipsShelvesThatAreTooTall") = []
{
    auto packer = ShelfPacker {128, 128, 1};

    const auto tall = packer.add(10, 40);
    const auto shortOne = packer.add(10, 6);

    check(tall.has_value());
    check(shortOne.has_value());
    check(shortOne->y != tall->y);
};

auto tRejectsOversizedRects = test("ShelfPacker/rejectsRectsLargerThanTheAtlas") = []
{
    auto packer = ShelfPacker {64, 64, 1};

    check(!packer.add(64, 10).has_value()); // no room for padding
    check(!packer.add(10, 64).has_value());
    check(!packer.add(200, 200).has_value());
    check(!packer.add(0, 10).has_value());
    check(!packer.add(10, -4).has_value());
};

auto tReportsFullWhenExhausted = test("ShelfPacker/returnsNothingOnceFull") = []
{
    auto packer = ShelfPacker {32, 32, 1};

    auto placed = 0;

    while (packer.add(10, 10).has_value())
    {
        ++placed;

        if (placed > 100)
            break; // a packer that never fills would hang the test
    }

    check(placed > 0);
    check(placed <= 9);
    check(!packer.add(10, 10).has_value());
};

// The property that matters most: nothing ever overlaps, across a long run of
// mixed sizes.
auto tNeverOverlaps = test("ShelfPacker/placementsNeverOverlap") = []
{
    constexpr auto atlasSize = 256;
    auto packer = ShelfPacker {atlasSize, atlasSize, 1};
    auto seen = Placements {};

    // Widths and heights that vary the way glyphs from one face do.
    const int widths[] = {4, 9, 12, 7, 20, 5, 11, 8};
    const int heights[] = {14, 14, 15, 14, 16, 14, 15, 14};

    for (auto i = 0; i < 400; ++i)
    {
        const auto w = widths[i % 8];
        const auto h = heights[i % 8];

        const auto placed = packer.add(w, h);

        if (!placed)
            break;

        check(seen.add(*placed, w, h, atlasSize));
    }

    check(seen.boxes.size() > 50);
};

auto tClearReleasesEverything = test("ShelfPacker/clearStartsOver") = []
{
    auto packer = ShelfPacker {64, 64, 1};

    while (packer.add(10, 10).has_value())
    {
    }

    packer.clear();

    const auto placed = packer.add(10, 10);

    check(placed.has_value());
    check(placed->x == 1);
    check(placed->y == 1);
};

// Growing must keep every existing placement valid — that is what lets the
// atlas double without re-rasterizing a single glyph.
auto tGrowKeepsPlacements = test("ShelfPacker/growKeepsExistingPlacements") = []
{
    auto packer = ShelfPacker {64, 64, 1};
    auto seen = Placements {};

    while (const auto placed = packer.add(10, 10))
        seen.add(*placed, 10, 10, 64);

    const auto before = seen.boxes.size();
    check(before > 0);

    packer.grow(128, 128);

    check(packer.width() == 128);
    check(packer.height() == 128);

    // New placements must not land on top of the old ones.
    while (const auto placed = packer.add(10, 10))
    {
        if (!seen.add(*placed, 10, 10, 128))
        {
            check(false); // overlapped something from before the grow
            break;
        }
    }

    check(seen.boxes.size() > before);
};

auto tGrowIgnoresShrinking = test("ShelfPacker/growNeverShrinks") = []
{
    auto packer = ShelfPacker {128, 128, 1};

    packer.grow(64, 64);

    check(packer.width() == 128);
    check(packer.height() == 128);
};

auto tOccupancyRises = test("ShelfPacker/occupancyRisesAsRectsArePacked") = []
{
    auto packer = ShelfPacker {128, 128, 1};

    check(packer.occupancy() == 0.f);

    for (auto i = 0; i < 20; ++i)
        packer.add(10, 10);

    const auto used = packer.occupancy();

    check(used > 0.f);
    check(used < 1.f);

    packer.clear();
    check(packer.occupancy() == 0.f);
};
