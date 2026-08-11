#include <eacp/UI/UI.h>

#include <NanoTest/NanoTest.h>

#include <cmath>
#include <string>

using namespace nano;
using namespace eacp;
using namespace eacp::UI;

namespace
{
bool hasDevice()
{
    return GPU::Device::shared().isValid();
}

bool overlaps(const CoverageAtlas::Slot& a, const CoverageAtlas::Slot& b)
{
    return a.x < b.x + b.width && b.x < a.x + a.width && a.y < b.y + b.height
           && b.y < a.y + a.height;
}

bool insideAtlas(const CoverageAtlas& atlas, const CoverageAtlas::Slot& slot)
{
    return slot.x >= 0 && slot.y >= 0 && slot.x + slot.width <= atlas.getWidth()
           && slot.y + slot.height <= atlas.getHeight();
}

// Every unmasked shape samples the opaque texel at the origin; a slot over it
// would multiply plain rectangles by stale coverage.
bool coversOpaqueCorner(const CoverageAtlas& atlas, const CoverageAtlas::Slot& slot)
{
    auto opaque = atlas.getOpaqueUV();
    auto extent = (float) atlas.getWidth();

    auto x = opaque.x * extent;
    auto y = opaque.y * extent;

    return x >= (float) slot.x && x < (float) (slot.x + slot.width)
           && y >= (float) slot.y && y < (float) (slot.y + slot.height);
}

// Leaves the atlas at its largest with the shelf past its last row, found by
// asking for masks that do not fit until it stops growing.
void fillToTheCeiling(CoverageAtlas& atlas)
{
    atlas.setRelocationAllowed(true);

    while (true)
    {
        auto size = atlas.getWidth();

        atlas.allocate(size, size);
        atlas.takeMovedFlag();

        if (atlas.getWidth() == size)
            break;
    }

    auto size = atlas.getWidth();

    atlas.allocate(size - 8, size - 8);
    atlas.takeMovedFlag();
    atlas.clearDroppedCount();
}
} // namespace

auto tPacksWithoutOverlap = test("CoverageAtlas/packsSlotsWithoutOverlapping") = []
{
    if (!hasDevice())
        return;

    auto atlas = CoverageAtlas {};
    auto placed = Vector<CoverageAtlas::Slot> {};

    for (auto i = 0; i < 200; ++i)
    {
        // Varied so rows end at different places: uniform slots pack correctly
        // by accident.
        auto width = 6 + (i * 13) % 37;
        auto height = 5 + (i * 7) % 23;

        auto slot = atlas.allocate(width, height);

        // A move invalidates every slot handed out before it.
        if (atlas.takeMovedFlag())
            placed.clear();

        check(slot.width == width, "asked for " + std::to_string(width));
        check(slot.height == height);
        check(insideAtlas(atlas, slot));
        check(!coversOpaqueCorner(atlas, slot));

        for (const auto& other: placed)
            check(!overlaps(slot, other), "slot " + std::to_string(i) + " overlaps");

        placed.add(slot);
    }
};

auto tUVMatchesSlot = test("CoverageAtlas/uvAddressesTheSlotItDescribes") = []
{
    if (!hasDevice())
        return;

    auto atlas = CoverageAtlas {};

    for (auto size: {20, 40, 90, 200, 500})
    {
        auto slot = atlas.allocate(size, size);
        atlas.takeMovedFlag();

        auto extent = (float) atlas.getWidth();

        check(std::abs(slot.uv.x * extent - (float) slot.x) < 0.01f);
        check(std::abs(slot.uv.y * extent - (float) slot.y) < 0.01f);
        check(std::abs(slot.uv.w * extent - (float) slot.width) < 0.01f);
        check(std::abs(slot.uv.h * extent - (float) slot.height) < 0.01f);
    }
};

auto tGrowsAndSaysSo = test("CoverageAtlas/growingReportsThatEverythingMoved") = []
{
    if (!hasDevice())
        return;

    auto atlas = CoverageAtlas {};
    auto size = atlas.getWidth();

    atlas.allocate(8, 8);
    check(!atlas.takeMovedFlag(), "a slot that fits moves nothing");

    auto slot = atlas.allocate(size * 2, size * 2);

    check(atlas.takeMovedFlag(), "growing has to be reported");
    check(atlas.getWidth() > size);
    check(slot.width == size * 2);
};

auto tCompactsAtTheCeiling = test("CoverageAtlas/compactsWhenItCannotGrow") = []
{
    if (!hasDevice())
        return;

    auto atlas = CoverageAtlas {};
    fillToTheCeiling(atlas);

    auto size = atlas.getWidth();
    auto slot = atlas.allocate(64, 64);

    check(atlas.takeMovedFlag(), "compacting has to be reported");
    check(atlas.getWidth() == size, "it was already as large as it goes");
    check(slot.width == 64);
    check(insideAtlas(atlas, slot));
};

auto tRefusesRatherThanMoving =
    test("CoverageAtlas/refusesRatherThanMovingWhenRelocationIsForbidden") = []
{
    if (!hasDevice())
        return;

    auto atlas = CoverageAtlas {};
    auto size = atlas.getWidth();

    auto first = atlas.allocate(16, 16);
    atlas.takeMovedFlag();

    atlas.setRelocationAllowed(false);
    atlas.clearDroppedCount();

    auto refused = atlas.allocate(size * 2, size * 2);

    check(refused.width == 0, "a mask that needs a move has to be refused");
    check(atlas.getDroppedCount() == 1);
    check(!atlas.takeMovedFlag(), "nothing moved, so nothing may say it did");
    check(atlas.getWidth() == size, "the atlas may not grow on this pass");

    auto next = atlas.allocate(16, 16);

    check(next.width == 16);
    check(!overlaps(first, next), "a refusal must not restart the shelf");
};

auto tCeilingIsCounted = test("CoverageAtlas/countsEveryMaskItHasNoRoomFor") = []
{
    if (!hasDevice())
        return;

    auto atlas = CoverageAtlas {};
    fillToTheCeiling(atlas);

    atlas.setRelocationAllowed(false);
    atlas.clearDroppedCount();

    for (auto i = 0; i < 5; ++i)
        check(atlas.allocate(256, 256).width == 0);

    check(atlas.getDroppedCount() == 5, "each one is a shape that draws as nothing");
    check(!atlas.takeMovedFlag());
};

auto tMaskLargerThanAnyAtlas =
    test("CoverageAtlas/refusesAndCountsAMaskLargerThanTheLargestAtlas") = []
{
    if (!hasDevice())
        return;

    auto atlas = CoverageAtlas {};
    atlas.setRelocationAllowed(true);
    atlas.clearDroppedCount();

    auto slot = atlas.allocate(64 * 1024, 64);

    check(slot.width == 0);
    check(atlas.getDroppedCount() == 1);
};

// A mask exactly as large as the atlas can never fit row one (the opaque corner
// owns four texels of it), so growing would loop for ever: it must be refused.
auto tMaskAsLargeAsTheAtlas =
    test("CoverageAtlas/refusesAMaskAsLargeAsTheAtlasRatherThanLoopingOnIt") = []
{
    if (!hasDevice())
        return;

    auto atlas = CoverageAtlas {};
    fillToTheCeiling(atlas);

    auto size = atlas.getWidth();

    atlas.setRelocationAllowed(true);
    atlas.clearDroppedCount();

    auto slot = atlas.allocate(size, size);

    check(slot.width == 0);
    check(atlas.getDroppedCount() == 1);
    check(atlas.getWidth() == size);
};

auto tForgettingMakesRoom = test("CoverageAtlas/forgettingAllocationsMakesRoom") = []
{
    if (!hasDevice())
        return;

    auto atlas = CoverageAtlas {};
    auto size = atlas.getWidth();

    atlas.allocate(size - 8, size - 8);
    atlas.takeMovedFlag();

    atlas.setRelocationAllowed(false);
    check(atlas.allocate(32, 32).width == 0, "the shelf is past its last row");

    atlas.forgetAllocations();

    auto slot = atlas.allocate(32, 32);

    check(slot.width == 32, "an empty shelf has room for it");
    check(atlas.getWidth() == size, "and no move was needed to find it");
};

auto tFillFraction = test("CoverageAtlas/fillFractionCountsTheRoomReserved") = []
{
    if (!hasDevice())
        return;

    auto atlas = CoverageAtlas {};
    auto extent = (float) atlas.getWidth();
    auto empty = atlas.getFillFraction();

    check(empty > 0.f, "the opaque corner is reserved too");
    check(empty < 0.05f);

    auto slot = atlas.allocate(20, 24);
    atlas.takeMovedFlag();

    auto expected = empty + (float) (slot.width * slot.height) / (extent * extent);

    check(std::abs(atlas.getFillFraction() - expected) < 0.001f);

    atlas.forgetAllocations();

    check(std::abs(atlas.getFillFraction() - empty) < 0.001f);
};
