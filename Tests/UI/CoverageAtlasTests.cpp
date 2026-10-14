#include "../GPUWidgets/CoverageProbe.h"
#include "Common.h"

#include <eacp/GPUWidgets/Path/CoverageKernel.h>
#include <eacp/UI/UI.h>
#include <NanoTest/NanoTest.h>

#include <cmath>
#include <optional>
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

// One channel where the device can store to one, which is a quarter of the
// memory of the RGBA8 it used to be at every size -- 16 MB rather than 64 at
// the ceiling. Asked of the device rather than assumed, so a D3D12 lane checks
// the fallback and a Metal one the R8.
auto tOneChannelWhereWritable =
    test("CoverageAtlas/isOneChannelWhereTheDeviceCanWriteOne") = []
{
    if (!hasDevice())
        return;

    auto atlas = CoverageAtlas {};
    auto oneChannel =
        GPU::Device::shared().supportsComputeWrite(GPU::TextureFormat::R8Unorm);

    check(atlas.getFormat()
          == (oneChannel ? GPU::TextureFormat::R8Unorm
                         : GPU::TextureFormat::RGBA8Unorm));
    check(atlas.getTexture().isValid());
    check(atlas.getTexture().isComputeWritable());

#if defined(__APPLE__)
    check(atlas.getFormat() == GPU::TextureFormat::R8Unorm, "every Metal GPU");
#endif

    // The opaque corner is a CPU upload at the texture's own stride, so it is
    // the part a change of format could get wrong without the kernel noticing.
    auto opaque = GPUWidgets::probe::readRegion(atlas.getTexture(), 0, 0, 4, 4);

    check(opaque.size() == 16);

    for (auto value: opaque)
        check(value == 1.f, "every texel of the opaque corner");
};

// A path rasterized into a slot of the atlas, read back, against the same path
// rasterized into a texture of its own: the coverage an atlas-backed shape
// samples is the coverage the path has, whichever format the atlas is in.

auto tCompactsBeforeGrowing =
    test("CoverageAtlas/compactsRatherThanGrowsWhenLittleOfItIsLive") = []
{
    if (!hasDevice())
        return;

    auto atlas = CoverageAtlas {};

    atlas.allocate(600, 600);
    atlas.takeMovedFlag();
    atlas.forgetAllocations();

    auto size = atlas.getWidth();
    auto tall = atlas.allocate(20, size - 24);

    atlas.release(tall);

    auto generation = atlas.generation();
    auto slot = atlas.allocate(size - 16, 40);

    check(atlas.takeMovedFlag(), "every slot handed out has moved");
    check(atlas.getWidth() == size, "compacted in place, not grown");
    check(atlas.generation() != generation);
    check(slot.width == size - 16);
    check(insideAtlas(atlas, slot));
};

auto tGrowsWhenCompactionCannotHelp =
    test("CoverageAtlas/growsWhenTheLiveSlotsStillDoNotFitAfterCompacting") = []
{
    if (!hasDevice())
        return;

    auto atlas = CoverageAtlas {};

    atlas.allocate(600, 600);
    atlas.takeMovedFlag();
    atlas.forgetAllocations();

    auto size = atlas.getWidth();

    atlas.allocate(20, size - 24);
    atlas.allocate(size - 16, 40);

    check(atlas.takeMovedFlag());
    check(atlas.getWidth() == size, "little is reserved, so it compacts first");

    atlas.forgetAllocations();
    atlas.allocate(20, size - 24);
    atlas.allocate(size - 16, 40);

    check(atlas.takeMovedFlag());
    check(atlas.getWidth() == size * 2,
          "and grows once the compaction was not enough");
};

auto tGrowsWhenFull =
    test("CoverageAtlas/growsRatherThanCompactsWhenMostOfItIsLive") = []
{
    if (!hasDevice())
        return;

    auto atlas = CoverageAtlas {};

    atlas.allocate(600, 600);
    atlas.takeMovedFlag();
    atlas.forgetAllocations();

    auto size = atlas.getWidth();
    auto quarter = size / 2 - 12;

    for (auto i = 0; i < 4; ++i)
        check(atlas.allocate(quarter, quarter).width == quarter);

    check(!atlas.takeMovedFlag(), "four quarters fit");
    check(atlas.getFillFraction() > 0.5f);

    atlas.allocate(quarter, quarter);

    check(atlas.takeMovedFlag());
    check(atlas.getWidth() == size * 2);
};

auto tShelvesByHeight = test("CoverageAtlas/slotsOfOneHeightShareAShelf") = []
{
    if (!hasDevice())
        return;

    auto atlas = CoverageAtlas {};

    atlas.allocate(600, 600);
    atlas.takeMovedFlag();
    atlas.forgetAllocations();

    auto size = atlas.getWidth();

    for (auto i = 0; i < 8; ++i)
    {
        atlas.allocate(size / 2 - 8, 200);
        atlas.allocate(size / 4, 20);
    }

    check(!atlas.takeMovedFlag(), "short masks do not each take a tall row");
};

auto tSlotReadsLikeSolo = test("CoverageAtlas/aPathReadsTheSameThroughTheAtlas") = []
{
    if (!hasDevice())
        return;

    auto path = GPUWidgets::Path {};
    path.addEllipse({0.f, 0.f, 37.f, 23.f});

    auto solo = GPUWidgets::PathRasterizer {};
    auto alone = GPUWidgets::probe::rasterize(solo, path, 2.f);

    check(!alone.empty());

    auto atlas = CoverageAtlas {};
    auto rasterizer = GPUWidgets::PathRasterizer {};
    rasterizer.setScale(2.f);
    rasterizer.setPath(path);

    auto width = rasterizer.getCoverageWidth();
    auto height = rasterizer.getCoverageHeight();

    auto slot = atlas.allocate(width, height);
    atlas.takeMovedFlag();

    check(slot.width == width);

    rasterizer.setTarget(atlas.getTexture(), slot.x, slot.y);

    auto batch = GPUWidgets::CoverageBatch {};
    batch.begin(atlas.getTexture());
    batch.add(rasterizer);

    auto commands = GPU::Device::shared().makeCommandBuffer();

    {
        auto pass = commands.beginCompute();
        batch.dispatch(pass);
    }

    commands.commit();

    auto through = GPUWidgets::probe::readRegion(
        atlas.getTexture(), slot.x, slot.y, width, height);

    check(through.size() == alone.size());

    if (through.size() != alone.size())
        return;

    for (auto i = 0; i < through.size(); ++i)
        check(through[i] == alone[i], "texel " + std::to_string(i));
};

namespace
{
// Asks once a walk, as the host does, until the quiet window has passed: true
// if the atlas shrank on any of those asks.
bool shrinkOnceQuiet(CoverageAtlas& atlas)
{
    auto walks = atlas.getQuietWalksToShrink();

    for (auto walk = 0; walk <= walks; ++walk)
        if (atlas.shrinkToFit())
            return true;

    return false;
}
} // namespace

// Room the shelf no longer needs goes back -- to the smallest size that holds
// the largest mask placed since it was emptied, with an eighth of it reserved
// at most -- and saying so the way a grow does, through the generation. Only
// after the quiet window, though: a grow just now is a reason to wait.
auto tShrinksToFit = test("CoverageAtlas/shrinksToWhatItHolds") = []
{
    if (!hasDevice())
        return;

    auto atlas = CoverageAtlas {};
    auto initial = atlas.getWidth();

    atlas.allocate(600, 600);
    atlas.takeMovedFlag();

    auto grown = atlas.getWidth();

    check(grown == 1024);
    check(!shrinkOnceQuiet(atlas), "full enough to keep");

    atlas.forgetAllocations();
    atlas.allocate(20, 20);

    auto generation = atlas.generation();

    atlas.setRelocationAllowed(false);
    check(!shrinkOnceQuiet(atlas), "a move, so not while moves are refused");
    check(atlas.getWidth() == grown);

    atlas.setRelocationAllowed(true);
    check(atlas.shrinkToFit(), "the window has long passed");
    check(atlas.getWidth() == initial, "an eighth of the smallest is room enough");
    check(atlas.generation() != generation, "every uv handed out is stale");
    check(!atlas.takeMovedFlag(), "nothing was placed in the new one to move");
    check(atlas.getTexture().isValid());

    // The largest mask is what bounds it, however little is reserved: a strip
    // as wide as a large atlas cannot go in a small one.
    atlas.allocate(700, 2);
    atlas.takeMovedFlag();

    check(atlas.getWidth() == 1024, "grown for the strip");

    atlas.forgetAllocations();
    atlas.allocate(700, 2);

    check(!shrinkOnceQuiet(atlas), "nothing smaller holds it");
    check(atlas.getWidth() == 1024);
};

// A slot churning under an animation -- a new mask every walk, the old one
// abandoned -- fills a small atlas in a few walks and makes it grow. A shrink
// right after every grow would be a resize, and a whole tree rasterized again,
// every few frames for as long as the animation runs. Played the way the host
// plays it: shrinkToFit before the walk, and the live set placed again after
// anything moves.
auto tChurnDoesNotCycle = test("CoverageAtlas/aChurningSlotDoesNotCycleTheSize") = []
{
    if (!hasDevice())
        return;

    auto atlas = CoverageAtlas {};

    auto placeLive = [&] { atlas.allocate(30, 30); };

    auto grows = 0;
    auto shrinks = 0;
    auto width = atlas.getWidth();

    placeLive();

    for (auto walk = 0; walk < 300; ++walk)
    {
        atlas.setRelocationAllowed(true);

        if (atlas.shrinkToFit())
            placeLive();

        atlas.allocate(60, 60);

        if (atlas.takeMovedFlag())
        {
            atlas.forgetAllocations();
            placeLive();
            atlas.allocate(60, 60);
            atlas.takeMovedFlag();
        }

        if (atlas.getWidth() > width)
            ++grows;
        else if (atlas.getWidth() < width)
            ++shrinks;

        width = atlas.getWidth();
    }

    check(shrinks == 0,
          std::to_string(shrinks) + " shrinks, each one grown straight out of");
    check(grows <= 5, std::to_string(grows) + " grows, one a doubling at most");
};

// And a shrink that is outgrown inside its window makes the next one wait
// twice as long, rather than none ever happening again.
auto tOutgrownShrinkBacksOff =
    test("CoverageAtlas/aShrinkOutgrownInsideItsWindowWaitsLongerNextTime") = []
{
    if (!hasDevice())
        return;

    auto atlas = CoverageAtlas {};
    auto window = atlas.getQuietWalksToShrink();

    atlas.allocate(200, 200);
    atlas.takeMovedFlag();

    auto grown = atlas.getWidth();

    atlas.forgetAllocations();
    atlas.allocate(10, 10);

    check(shrinkOnceQuiet(atlas));
    check(atlas.getWidth() < grown);

    atlas.allocate(200, 200);
    atlas.takeMovedFlag();

    check(atlas.getWidth() == grown, "grown straight back");
    check(atlas.getQuietWalksToShrink() == window * 2, "so the next waits longer");

    atlas.forgetAllocations();
    atlas.allocate(10, 10);

    for (auto walk = 0; walk < window + 1; ++walk)
        check(!atlas.shrinkToFit(), "not inside the old window");

    check(shrinkOnceQuiet(atlas), "but still once the new one has passed");
};

namespace
{
// A component that fills one path, held as a mask whatever its size so it is
// always the atlas that pays for it.
GPUWidgets::Path rectPath(const Rect& area)
{
    auto path = GPUWidgets::Path {};
    path.addRect(area);
    return path;
}

struct Filled final : Component
{
    explicit Filled(const Rect& area)
        : Filled(rectPath(area))
    {
    }

    explicit Filled(const GPUWidgets::Path& path,
                    GPUWidgets::FillRule rule = GPUWidgets::FillRule::NonZero)
        : shape(*this)
    {
        shape.setBacking(PathShape::Backing::Mask);
        shape.setPath(path, rule);
    }

    void paint(UI::Graphics& g) override
    {
        g.setColour(Color {1.f, 0.f, 0.f, 1.f});
        g.fillPath(shape);
    }

    PathShape shape;
};

struct HiddenPage final : Component
{
    HiddenPage()
        : shown({0.f, 0.f, 10.f, 10.f})
        , hidden({0.f, 0.f, 300.f, 300.f})
    {
        addAndMakeVisible(shown);
        addChildComponent(hidden);
        hidden.setVisible(false);
    }

    void resized() override
    {
        shown.setBounds({0.f, 0.f, 10.f, 10.f});
        hidden.setBounds({20.f, 20.f, 300.f, 300.f});
    }

    Filled shown;
    Filled hidden;
};
} // namespace

// A hidden subtree is not drawn, so it takes no room in the atlas -- a page
// nobody has opened, five of six states of a toggle. And it is not forgotten
// either: the frame it is shown on rasterizes it, before anything records it.
auto tHiddenTakesNoSlot =
    test("CoverageAtlas/aHiddenSubtreeTakesNoRoomUntilItIsShown") = []
{
    if (!hasDevice())
        return;

    auto page = HiddenPage {};
    auto host = ComponentHost {};

    host.setBackgroundColour(Color::white());
    host.setBounds({0.f, 0.f, 400.f, 400.f});
    host.resized();
    host.setRootComponent(page);
    page.setBounds({0.f, 0.f, 400.f, 400.f});

    (void) host.renderToImage(1.f);

    check(!page.shown.shape.isEmpty(), "the visible one is rasterized");
    check(page.hidden.shape.isEmpty(), "the hidden one is not");
    check(host.getAtlasSize() < 512,
          "and the atlas has not grown to hold 300 pixels square nobody sees");

    page.hidden.setVisible(true);

    auto image = host.renderToImage(1.f);

    check(!page.hidden.shape.isEmpty(), "rasterized on the frame it is shown");
    check(host.getAtlasSize() >= 512);

    auto pixel = image.at(170, 170);

    check(pixel.r > 0.9f && pixel.g < 0.1f, "and drawn on that frame too");
};

namespace
{
// A component whose only drawing is a layer, filled with its own path -- the
// shape an SVG opacity group is.
struct Layered final : Component
{
    Layered()
        : shape(*this)
        , layer(*this)
    {
        shape.setBacking(PathShape::Backing::Mask);
        shape.setPath(rectPath({0.f, 0.f, 20.f, 20.f}));

        layer.setBounds({0.f, 0.f, 20.f, 20.f});
        layer.onPaint = [this](UI::Graphics& g)
        {
            g.setColour(Color {1.f, 0.f, 0.f, 1.f});
            g.fillPath(shape);
        };
    }

    void paint(UI::Graphics& g) override { g.drawLayer(layer); }

    PathShape shape;
    Layer layer;
};

bool isRedAt(const eacp::Graphics::Image& image, int x, int y)
{
    auto pixel = image.at(x, y);
    return pixel.r > 0.9f && pixel.g < 0.1f && pixel.b < 0.1f;
}

void settle(ComponentHost& host, Component& root, float size)
{
    host.setBackgroundColour(Color::white());
    host.setBounds({0.f, 0.f, size, size});
    host.resized();
    host.setRootComponent(root);
    root.setBounds({0.f, 0.f, size, size});
}
} // namespace

// A layer belonging to a hidden component is not rendered while its shapes have
// no masks: rendered then, it would be empty and marked done, and showing the
// component would rasterize the shapes and leave the layer as it was.
auto tHiddenLayerRendersWhenShown =
    test("CoverageAtlas/aHiddenComponentsLayerIsRenderedOnTheFrameItIsShown") = []
{
    if (!hasDevice())
        return;

    struct Root final : Component
    {
        Root()
        {
            addAndMakeVisible(group);
            group.setVisible(false);
        }

        void resized() override { group.setBounds({10.f, 10.f, 20.f, 20.f}); }

        Layered group;
    };

    auto root = Root {};
    auto host = ComponentHost {};
    settle(host, root, 64.f);

    (void) host.renderToImage(1.f);

    root.group.setVisible(true);

    auto image = host.renderToImage(1.f);

    check(isRedAt(image, 20, 20), "the group, filled, on the frame it is shown");
};

// The other way a hidden layer goes stale: rendered while visible, hidden, the
// atlas moves under its shape, and then the layer is dirtied while still hidden.
// Rendered then, it would draw through the uv its shape had before the move --
// into texels that now belong to a shape with a hole there.
auto tHiddenLayerSurvivesAMove =
    test("CoverageAtlas/aLayerDirtiedWhileHiddenAcrossAMoveDrawsItsOwnShape") = []
{
    if (!hasDevice())
        return;

    struct Root final : Component
    {
        Root()
            : small({0.f, 0.f, 10.f, 10.f})
            , ring(ringPath(), GPUWidgets::FillRule::EvenOdd)
        {
            addAndMakeVisible(group);
            addAndMakeVisible(small);
            addAndMakeVisible(ring);
            ring.setVisible(false);
        }

        static GPUWidgets::Path ringPath()
        {
            auto path = GPUWidgets::Path {};
            path.addRect({0.f, 0.f, 600.f, 600.f});
            path.addRect({20.f, 20.f, 560.f, 560.f});
            return path;
        }

        void resized() override
        {
            group.setBounds({10.f, 10.f, 30.f, 30.f});
            small.setBounds({50.f, 10.f, 10.f, 10.f});
            ring.setBounds({100.f, 100.f, 600.f, 600.f});
        }

        Layered group;
        Filled small;
        Filled ring;
    };

    auto root = Root {};
    auto host = ComponentHost {};
    settle(host, root, 720.f);

    auto first = host.renderToImage(1.f);
    auto before = host.getAtlasSize();

    check(isRedAt(first, 20, 20), "drawn while it was visible");

    root.group.setVisible(false);
    root.ring.setVisible(true);
    (void) host.renderToImage(1.f);

    check(host.getAtlasSize() > before, "the atlas grew, and everything moved");

    root.group.layer.setBounds({0.f, 0.f, 22.f, 22.f});
    (void) host.renderToImage(1.f);

    root.group.setVisible(true);

    auto image = host.renderToImage(1.f);

    check(isRedAt(image, 20, 20), "its own coverage, not the ring's hole");
};

// A hidden subtree whose shapes held slots before the atlas moved: shown again,
// they are rasterized into the atlas as it is now and drawn from there.
auto tHiddenShapesSurviveAMove =
    test("CoverageAtlas/aSubtreeHiddenAcrossAMoveDrawsWhenShown") = []
{
    if (!hasDevice())
        return;

    struct Root final : Component
    {
        Root()
            : held({0.f, 0.f, 20.f, 20.f})
            , ring(ringPath(), GPUWidgets::FillRule::EvenOdd)
        {
            addAndMakeVisible(held);
            addAndMakeVisible(ring);
            ring.setVisible(false);
        }

        static GPUWidgets::Path ringPath()
        {
            auto path = GPUWidgets::Path {};
            path.addRect({0.f, 0.f, 600.f, 600.f});
            path.addRect({20.f, 20.f, 560.f, 560.f});
            return path;
        }

        void resized() override
        {
            held.setBounds({10.f, 10.f, 20.f, 20.f});
            ring.setBounds({100.f, 100.f, 600.f, 600.f});
        }

        Filled held;
        Filled ring;
    };

    auto root = Root {};
    auto host = ComponentHost {};
    settle(host, root, 720.f);

    (void) host.renderToImage(1.f);
    auto before = host.getAtlasSize();

    root.held.setVisible(false);
    root.ring.setVisible(true);
    (void) host.renderToImage(1.f);

    check(host.getAtlasSize() > before);

    root.held.setVisible(true);

    auto image = host.renderToImage(1.f);

    check(isRedAt(image, 20, 20));
    check(isRedAt(image, 105, 105), "and the ring that moved it");
    check(!isRedAt(image, 400, 400), "with its hole");
};

// The shrink as the host plays it: once the window has passed with a small live
// set, the atlas is rebuilt smaller before the walk, every shape is rasterized
// into it on that same frame and the tree recorded again -- so the frame it
// happens on draws what it should.
auto tHostShrinkDrawsTheSameFrame =
    test("CoverageAtlas/aHostShrinksTheAtlasAndDrawsTheFrameItDoesSoOn") = []
{
    if (!hasDevice())
        return;

    struct Root final : Component
    {
        Root()
            : tall({0.f, 0.f, 600.f, 1000.f})
            , strip({0.f, 0.f, 1000.f, 30.f})
        {
            addAndMakeVisible(tall);
        }

        void resized() override
        {
            tall.setBounds({0.f, 0.f, 600.f, 1000.f});
            strip.setBounds({10.f, 10.f, 1000.f, 30.f});
        }

        Filled tall;
        Filled strip;
    };

    auto root = Root {};
    auto host = ComponentHost {};
    host.setAtlasQuietWalksToShrink(3);
    settle(host, root, 1040.f);

    (void) host.renderToImage(1.f);

    check(host.getAtlasSize() == 1024);

    // The tall shape goes and the strip arrives, which does not fit beside the
    // tall one's slot -- so the atlas grows and is filled again with the strip
    // alone, a sixteenth of the grown size.
    root.removeChildComponent(root.tall);
    root.addAndMakeVisible(root.strip);

    (void) host.renderToImage(1.f);

    check(host.getAtlasSize() == 2048);

    auto shrankOn = std::optional<eacp::Graphics::Image> {};

    for (auto frame = 0; frame < 10 && !shrankOn; ++frame)
    {
        auto image = host.renderToImage(1.f);

        if (host.getAtlasSize() < 2048)
            shrankOn = image;
    }

    check(shrankOn.has_value(), "shrunk once the window passed");
    check(host.getAtlasSize() == 1024, "to the size the strip needs");

    if (!shrankOn)
        return;

    check(isRedAt(*shrankOn, 500, 25),
          "the strip, drawn on the frame of the shrink");
    check(!isRedAt(*shrankOn, 500, 60), "and nothing else");
};

namespace
{
struct Panel final : Component
{
    explicit Panel(int index)
    {
        add({30.f, 200.f + (float) index});

        for (auto i = 0; i < 10; ++i)
        {
            auto step = (float) (index * 10 + i);

            add({60.f + step * 0.5f, 60.f + step * 0.5f});
            add({20.f + step * 0.25f, 20.f + step * 0.25f});
        }
    }

    void add(Point size)
    {
        sizes.add(size);
        addAndMakeVisible(shapes.createNew(Rect {0.f, 0.f, size.x, size.y}));
    }

    void scaleShapes(float factor)
    {
        for (auto i = 0; i < shapes.size(); ++i)
            shapes[i]->shape.setPath(
                rectPath({0.f, 0.f, sizes[i].x * factor, sizes[i].y * factor}));
    }

    Vector<Point> sizes;
    OwnedVector<Filled> shapes;
};

struct Panels final : Component
{
    explicit Panels(int count)
    {
        for (auto i = 0; i < count; ++i)
            addAndMakeVisible(panels.createNew(i));
    }

    void resized() override
    {
        for (auto& panel: panels)
            panel->setBounds(getLocalBounds());
    }

    void scaleShapes(float factor)
    {
        for (auto& panel: panels)
            panel->scaleShapes(factor);
    }

    OwnedVector<Panel> panels;
};

GPUWidgets::Path ellipsePath(const Rect& area)
{
    auto path = GPUWidgets::Path {};
    path.addEllipse(area);
    return path;
}

bool sameRegion(const eacp::Graphics::Image& a,
                const eacp::Graphics::Image& b,
                const Rect& area)
{
    for (auto y = (int) area.y; y < (int) (area.y + area.h); ++y)
    {
        for (auto x = (int) area.x; x < (int) (area.x + area.w); ++x)
        {
            auto p = a.at(x, y);
            auto q = b.at(x, y);

            if (p.r != q.r || p.g != q.g || p.b != q.b || p.a != q.a)
                return false;
        }
    }

    return true;
}
} // namespace

auto tResizedTreeCompacts =
    test("CoverageAtlas/aResizedTreeCompactsRatherThanGrowingPastWhatItHolds") = []
{
    if (!hasDevice())
        return;

    auto root = Panels {4};
    auto host = ComponentHost {};
    settle(host, root, 818.f);

    (void) host.renderToImage(2.f);

    check(host.getAtlasSize() == 2048);

    root.scaleShapes(1.2f);
    (void) host.renderToImage(2.f);

    check(host.getAtlasSize() == 2048,
          "grown to " + std::to_string(host.getAtlasSize()) + " at "
              + std::to_string(host.getAtlasFillFraction()) + " full");
    check(host.getLastDroppedPathCount() == 0);
};

auto tSeveralGrowsInOneWalk =
    test("CoverageAtlas/aTreeThatGrowsTheAtlasSeveralTimesDropsNothing") = []
{
    if (!hasDevice())
        return;

    auto root = Panels {6};
    auto host = ComponentHost {};
    settle(host, root, 818.f);

    (void) host.renderToImage(2.f);

    check(host.getLastDroppedPathCount() == 0,
          std::to_string(host.getLastDroppedPathCount()) + " dropped");
};

auto tCompactionKeepsUVs =
    test("CoverageAtlas/aShapeDrawsTheSamePixelsAfterTheAtlasCompacts") = []
{
    if (!hasDevice())
        return;

    struct Root final : Component
    {
        Root()
            : growing(rectPath({0.f, 0.f, 10.f, 10.f}))
            , kept(ellipsePath({0.f, 0.f, 97.f, 61.f}))
        {
            addAndMakeVisible(growing);
            addAndMakeVisible(kept);
        }

        void resized() override
        {
            growing.setBounds({0.f, 100.f, 400.f, 300.f});
            kept.setBounds({10.f, 10.f, 97.f, 61.f});
        }

        Filled growing;
        Filled kept;
    };

    auto root = Root {};
    auto host = ComponentHost {};
    settle(host, root, 400.f);

    auto before = host.renderToImage(1.f);
    auto keptArea = Rect {10.f, 10.f, 97.f, 61.f};

    check(isRedAt(before, 58, 40), "the ellipse is drawn");

    auto compactedOn = std::optional<eacp::Graphics::Image> {};
    auto size = host.getAtlasSize();
    auto fill = host.getAtlasFillFraction();

    for (auto step = 1; step < 200 && !compactedOn; ++step)
    {
        auto edge = 10.f + (float) step * 2.f;
        root.growing.shape.setPath(rectPath({0.f, 0.f, edge, edge * 0.5f}));

        auto image = host.renderToImage(1.f);

        if (host.getAtlasSize() == size && host.getAtlasFillFraction() < fill)
            compactedOn = image;

        size = host.getAtlasSize();
        fill = host.getAtlasFillFraction();
    }

    check(compactedOn.has_value(), "the atlas compacted");

    if (!compactedOn)
        return;

    check(sameRegion(before, *compactedOn, keptArea),
          "the ellipse reads its own texels through its new uv");
};
