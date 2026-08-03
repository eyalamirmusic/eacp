#include <eacp/UI/UI.h>

#include <NanoTest/NanoTest.h>

#include <cmath>
#include <string>

// What the shelf hands out, and what it does when it cannot.
//
// The interesting half is the second one. An atlas that runs out has two ways to
// make room and both of them move every slot already handed out, so a caller can
// only survive one while it can still rasterize the whole tree again. On the
// pass whose layout is the one being drawn through it cannot -- and a move there
// is not a shape going missing, it is a shape drawing through texels that now
// belong to somebody else. Nothing crashes, nothing is logged, and the picture
// is wrong.
//
// So these check the two outcomes apart: with relocation allowed, an allocation
// that does not fit moves everything and says so; with it forbidden, the same
// allocation is refused, counted, and leaves every existing slot exactly where
// it was. The second is the one that could not previously be asked for.

using namespace nano;
using namespace eacp;
using namespace eacp::UI;

namespace
{
// The atlas owns a texture, so it needs a device - but no window, no pass and no
// dispatch: allocation is arithmetic over a rect.
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

// The opaque texel every unmasked shape samples lives at the origin. A slot
// handed out over it would make every plain rectangle in the interface multiply
// by whatever coverage landed there instead of by one.
bool coversOpaqueCorner(const CoverageAtlas& atlas, const CoverageAtlas::Slot& slot)
{
    auto opaque = atlas.getOpaqueUV();
    auto extent = (float) atlas.getWidth();

    auto x = opaque.x * extent;
    auto y = opaque.y * extent;

    return x >= (float) slot.x && x < (float) (slot.x + slot.width)
           && y >= (float) slot.y && y < (float) (slot.y + slot.height);
}

// Leaves the atlas at its largest with the shelf past its last row, which is
// the only state the ceiling can be reached from. Deliberately not written
// against the size constants: it asks the atlas how large it is, and finds the
// largest by asking for masks that do not fit until it stops growing.
void fillToTheCeiling(CoverageAtlas& atlas)
{
    atlas.setRelocationAllowed(true);

    // A mask as large as the whole atlas never fits one, the opaque corner
    // owning the left-hand end of the first row - so this grows it by a doubling
    // a time and stops when a doubling stops happening.
    while (true)
    {
        auto size = atlas.getWidth();

        atlas.allocate(size, size);
        atlas.takeMovedFlag();

        if (atlas.getWidth() == size)
            break;
    }

    // Then one mask that does fit, and fills it: the row it is on has eight
    // texels of width left and nothing can start below it.
    auto size = atlas.getWidth();

    atlas.allocate(size - 8, size - 8);
    atlas.takeMovedFlag();
    atlas.clearDroppedCount();
}
} // namespace

// The plain case, and the one every frame that does not run out is made of.
auto tPacksWithoutOverlap = test("CoverageAtlas/packsSlotsWithoutOverlapping") = []
{
    if (!hasDevice())
        return;

    auto atlas = CoverageAtlas {};
    auto placed = Vector<CoverageAtlas::Slot> {};

    for (auto i = 0; i < 200; ++i)
    {
        // Varied enough that rows end at different places, since a shelf whose
        // slots are all one size packs correctly by accident.
        auto width = 6 + (i * 13) % 37;
        auto height = 5 + (i * 7) % 23;

        auto slot = atlas.allocate(width, height);

        // A move relocates everything handed out before it, so the record
        // starts again rather than comparing against slots that no longer mean
        // anything - which is what a caller has to do too.
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

// The uv is what the quad samples, so a slot whose rect and uv disagree draws
// the right shape through the wrong texels - and after a grow the same rect
// means a different uv, which is the case that is easy to get wrong.
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

// Growing is the first answer to running out, and it is only sound because it
// admits to it: the caller re-rasterizes everything against the new size.
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

// The ceiling, with a move still permitted: the atlas is as large as it goes, so
// it compacts, which is the other move and equally something the caller has to
// hear about.
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

// The one this file exists for. On the pass that has to end with a consistent
// atlas, an allocation that would move something is refused - so what is already
// placed stays exactly where the shapes holding it think it is.
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

    // And the proof that it did not: a slot handed out afterwards still avoids
    // the one handed out before. A grow or a compact here would have restarted
    // the shelf on top of it, and both shapes would sample the same texels.
    auto next = atlas.allocate(16, 16);

    check(next.width == 16);
    check(!overlaps(first, next), "a refusal must not restart the shelf");
};

// The tree that genuinely does not fit: the atlas is at its largest, already
// full, and may not compact because compacting is what the pass before it did.
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

// A single mask no atlas could hold. Refused even with a move allowed, since
// there is no move that would help, and counted like any other thing missing
// from the picture.
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

// The mask that did not come back. Exactly as large as the atlas itself, so
// room is made for it, it still does not fit the first row - the opaque corner
// owning four texels of it - and making room again is what the shelf did next,
// for ever. It is a refusal, and there is no arrangement of this atlas under
// which it is anything else.
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

// What the caller does between the two passes: everything is about to be
// rasterized again, so nothing holds a slot and the shelf starts empty. Without
// it the second pass allocates beside what the first one abandoned.
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

// The distance to the ceiling, while there is still distance to it. Room
// reserved rather than room drawn into, because it is the reservation that runs
// out - and the shelf never gives any of it back.
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
