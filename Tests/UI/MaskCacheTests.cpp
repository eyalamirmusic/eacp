#include <eacp/UI/UI.h>

#include <NanoTest/NanoTest.h>

// Who is allowed to write into a slot, which is the whole of this class.
//
// Sharing a mask is easy; sharing one that somebody is still allowed to
// rasterize over is a picture where one shape draws through another's quad, and
// it fails silently. So the rule is that publishing an entry is an offer to give
// up the slot, and taking one accepts it for good: after a take, the publisher
// cannot have it back and has to allocate elsewhere.
//
// That is what makes the animated case survivable. A shape whose geometry
// changes takes its own offer back on the first change -- costing nothing at all
// while nobody shared it -- and stops publishing, so it goes on rewriting one
// private slot the way it did before any of this existed.
//
// None of it needs a GPU: an entry is a rect, a uv and a claim.

using namespace nano;
using namespace eacp;
using namespace eacp::UI;

namespace
{
GPUWidgets::Path rect(const Rect& bounds)
{
    auto path = GPUWidgets::Path {};
    path.addRect(bounds);

    return path;
}

MaskCache::Entry entryAt(int x, int y)
{
    auto entry = MaskCache::Entry {};
    entry.slot = {x, y, 10, 10, {}};
    entry.maskUV = {(float) x, (float) y, 10.f, 10.f};
    entry.bounds = {0.f, 0.f, 10.f, 10.f};

    return entry;
}

constexpr auto someKey = std::uint64_t {0x9e3779b97f4a7c15ull};
constexpr auto otherKey = std::uint64_t {0xc2b2ae3d27d4eb4full};
} // namespace

auto tEmptyIsAMiss = test("MaskCache/anEmptyCacheHoldsNothing") = []
{
    auto cache = MaskCache {};

    check(cache.take(
              someKey, rect({0.f, 0.f, 10.f, 10.f}), GPUWidgets::FillRule::NonZero)
          == nullptr);
    check(cache.getEntryCount() == 0);
    check(cache.getSharedCount() == 0);
};

auto tPublishedIsFound = test("MaskCache/aPublishedMaskIsHandedToTheNextAsker") = []
{
    auto cache = MaskCache {};
    auto shape = rect({0.f, 0.f, 10.f, 10.f});

    cache.publish(someKey, shape, GPUWidgets::FillRule::NonZero, entryAt(4, 8));

    check(cache.getEntryCount() == 1);

    const auto* hit = cache.take(someKey, shape, GPUWidgets::FillRule::NonZero);

    check(hit != nullptr);
    check(hit->slot.x == 4 && hit->slot.y == 8, "their slot, not a copy of it");
    check(hit->maskUV.x == 4.f);
    check(cache.getSharedCount() == 1, "and that is one mask not rasterized");
};

// The key is a summary, so a match on it is checked against the geometry before
// anything is shared. Two shapes colliding on a key is not two shapes.
auto tCollisionIsAMiss = test("MaskCache/aKeyThatMatchesTheWrongShapeIsAMiss") = []
{
    auto cache = MaskCache {};

    cache.publish(someKey,
                  rect({0.f, 0.f, 10.f, 10.f}),
                  GPUWidgets::FillRule::NonZero,
                  entryAt(4, 8));

    // The same key presented with different geometry, which is what a collision
    // looks like from in here.
    check(cache.take(
              someKey, rect({0.f, 0.f, 10.f, 20.f}), GPUWidgets::FillRule::NonZero)
          == nullptr);

    check(cache.getSharedCount() == 0, "nothing was shared, so nothing was saved");
    check(cache.getEntryCount() == 1, "and the entry that was there is still there");
};

auto tFillRuleIsCompared =
    test("MaskCache/theSamePointsUnderADifferentRuleAreNotTheSameMask") = []
{
    auto cache = MaskCache {};
    auto shape = rect({0.f, 0.f, 10.f, 10.f});

    cache.publish(someKey, shape, GPUWidgets::FillRule::NonZero, entryAt(4, 8));

    check(cache.take(someKey, shape, GPUWidgets::FillRule::EvenOdd) == nullptr);
};

// The offer, taken back. Nobody shared it, so the publisher may rasterize over
// the slot again -- which is the whole of what keeps an animating shape on one
// slot instead of one per frame.
auto tReclaimUntaken = test("MaskCache/anOfferNobodyTookCanBeWithdrawn") = []
{
    auto cache = MaskCache {};
    auto shape = rect({0.f, 0.f, 10.f, 10.f});

    cache.publish(someKey, shape, GPUWidgets::FillRule::NonZero, entryAt(4, 8));

    check(cache.reclaim(someKey));
    check(cache.getEntryCount() == 0);

    check(cache.take(someKey, shape, GPUWidgets::FillRule::NonZero) == nullptr,
          "and it is gone rather than merely spoken for");
};

auto tReclaimTaken = test("MaskCache/anOfferSomebodyTookCannotBeWithdrawn") = []
{
    auto cache = MaskCache {};
    auto shape = rect({0.f, 0.f, 10.f, 10.f});

    cache.publish(someKey, shape, GPUWidgets::FillRule::NonZero, entryAt(4, 8));
    cache.take(someKey, shape, GPUWidgets::FillRule::NonZero);

    // The taker recorded that uv into its own draw list and there is no asking
    // for it back, so the slot is theirs and the publisher has to move.
    check(!cache.reclaim(someKey));
    check(cache.getEntryCount() == 1);

    check(cache.take(someKey, shape, GPUWidgets::FillRule::NonZero) != nullptr,
          "and everybody after them still shares it");
};

auto tReclaimUnknown = test("MaskCache/withdrawingSomethingNeverOfferedFails") = []
{
    auto cache = MaskCache {};

    check(!cache.reclaim(someKey));
};

// A second offer under a key already spoken for is the publisher offering the
// same thing twice, or a collision. Either way the entry already there is the
// one with holders, so it is the one to keep.
auto tPublishDoesNotOverwrite =
    test("MaskCache/anOfferDoesNotDisplaceTheOneAlreadyThere") = []
{
    auto cache = MaskCache {};
    auto shape = rect({0.f, 0.f, 10.f, 10.f});

    cache.publish(someKey, shape, GPUWidgets::FillRule::NonZero, entryAt(4, 8));
    cache.publish(someKey, shape, GPUWidgets::FillRule::NonZero, entryAt(99, 99));

    const auto* hit = cache.take(someKey, shape, GPUWidgets::FillRule::NonZero);

    check(hit != nullptr);
    check(hit->slot.x == 4, "the first one, which is the one anybody could hold");
    check(cache.getEntryCount() == 1);
};

auto tDistinctShapes = test("MaskCache/twoShapesAreTwoEntries") = []
{
    auto cache = MaskCache {};

    cache.publish(someKey,
                  rect({0.f, 0.f, 10.f, 10.f}),
                  GPUWidgets::FillRule::NonZero,
                  entryAt(4, 8));

    cache.publish(otherKey,
                  rect({0.f, 0.f, 20.f, 20.f}),
                  GPUWidgets::FillRule::NonZero,
                  entryAt(20, 8));

    check(cache.getEntryCount() == 2);

    const auto* hit = cache.take(
        otherKey, rect({0.f, 0.f, 20.f, 20.f}), GPUWidgets::FillRule::NonZero);

    check(hit != nullptr);
    check(hit->slot.x == 20);
};

// Coverage is device pixels, so a mask rasterized at one scale is the wrong
// size for a shape drawn at another. Nothing is keyed by scale; the cache is
// simply dropped, which is also what the atlas behind it does.
auto tScaleDropsEverything =
    test("MaskCache/aChangeOfDeviceScaleDropsEveryMask") = []
{
    auto cache = MaskCache {};
    auto shape = rect({0.f, 0.f, 10.f, 10.f});

    cache.setScale(2.f);
    cache.publish(someKey, shape, GPUWidgets::FillRule::NonZero, entryAt(4, 8));

    cache.setScale(2.f);

    check(cache.getEntryCount() == 1, "the same scale is not a change");

    cache.setScale(1.f);

    check(cache.getEntryCount() == 0);
    check(cache.take(someKey, shape, GPUWidgets::FillRule::NonZero) == nullptr);
};

auto tClear = test("MaskCache/clearingDropsEveryEntry") = []
{
    auto cache = MaskCache {};
    auto shape = rect({0.f, 0.f, 10.f, 10.f});

    cache.publish(someKey, shape, GPUWidgets::FillRule::NonZero, entryAt(4, 8));
    cache.take(someKey, shape, GPUWidgets::FillRule::NonZero);

    cache.clear();

    check(cache.getEntryCount() == 0, "including the ones somebody took");
    check(cache.take(someKey, shape, GPUWidgets::FillRule::NonZero) == nullptr);
};

auto tSharedCount = test("MaskCache/theSharedCountIsWhatWasNotRasterized") = []
{
    auto cache = MaskCache {};
    auto shape = rect({0.f, 0.f, 10.f, 10.f});

    cache.publish(someKey, shape, GPUWidgets::FillRule::NonZero, entryAt(4, 8));

    for (auto i = 0; i < 47; ++i)
        check(cache.take(someKey, shape, GPUWidgets::FillRule::NonZero) != nullptr);

    // Forty-eight channel strips drawing one knob arc: one rasterization, and
    // forty-seven quads sampling what it wrote.
    check(cache.getSharedCount() == 47);
    check(cache.getEntryCount() == 1);

    cache.clearSharedCount();

    check(cache.getSharedCount() == 0);
};
