#include <eacp/UI/UI.h>

#include <NanoTest/NanoTest.h>

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

auto tCollisionIsAMiss = test("MaskCache/aKeyThatMatchesTheWrongShapeIsAMiss") = []
{
    auto cache = MaskCache {};

    cache.publish(someKey,
                  rect({0.f, 0.f, 10.f, 10.f}),
                  GPUWidgets::FillRule::NonZero,
                  entryAt(4, 8));

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

    check(cache.getSharedCount() == 47);
    check(cache.getEntryCount() == 1);

    cache.clearSharedCount();

    check(cache.getSharedCount() == 0);
};
