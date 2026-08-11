#include <eacp/UI/Render/ClipShader.h>

#include <NanoTest/NanoTest.h>

using namespace nano;
using namespace eacp;
using namespace eacp::UI;

namespace
{
// The arithmetic the fragment stage does with the two packed uniforms.
Point placeOf(const Array<float, 4>& region, Point position)
{
    return {(position.x - region[0]) * region[2],
            (position.y - region[1]) * region[3]};
}

bool isInside(Point place)
{
    return std::abs(place.x - 0.5f) <= 0.5f && std::abs(place.y - 0.5f) <= 0.5f;
}

const auto opaque = Rect {0.25f, 0.75f, 0.f, 0.f};
} // namespace

auto tNoClipReadsOne = test("ClipMask/noClipIsAValueRatherThanABranch") = []
{
    auto region = Array<float, 4> {};
    auto mask = Array<float, 4> {};

    packClipMask({}, opaque, region, mask);

    for (auto point: {Point {0.f, 0.f}, Point {1920.f, 1080.f}, Point {-40.f, 12.f}})
    {
        check(isInside(placeOf(region, point)),
              "an unclipped fragment is never cut");
        check(placeOf(region, point).x == 0.f, "and reads one fixed texel");
    }

    check(mask[0] == opaque.x);
    check(mask[1] == opaque.y);
    check(mask[2] == 0.f);
    check(mask[3] == 0.f);
};

auto tClipMapsItsOwnBox = test("ClipMask/aRegionMapsItsBoxOntoItsAtlasRect") = []
{
    auto region = Array<float, 4> {};
    auto mask = Array<float, 4> {};

    auto clip = ClipMask {{100.f, 200.f, 50.f, 20.f}, {0.5f, 0.25f, 0.1f, 0.05f}};

    packClipMask(clip, opaque, region, mask);

    check(isInside(placeOf(region, {100.f, 200.f})), "the first corner is inside");
    check(isInside(placeOf(region, {150.f, 220.f})), "and so is the last");
    check(!isInside(placeOf(region, {99.f, 205.f})), "a fragment left of it is not");
    check(!isInside(placeOf(region, {120.f, 221.f})), "nor one below it");

    auto middle = placeOf(region, {125.f, 210.f});

    check(middle.x == 0.5f && middle.y == 0.5f);

    check(mask[0] + middle.x * mask[2] == 0.55f);
    check(mask[1] + middle.y * mask[3] == 0.275f);
};

auto tEmptyRegion = test("ClipMask/aRegionOfNoSizeIsNoClip") = []
{
    check(ClipMask {}.isEmpty());
    check(ClipMask {{10.f, 10.f, 0.f, 5.f}, {}}.isEmpty(), "no width");
    check(!ClipMask {{10.f, 10.f, 5.f, 5.f}, {}}.isEmpty());
};
