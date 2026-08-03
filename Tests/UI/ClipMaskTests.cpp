#include <eacp/UI/Render/ClipShader.h>

#include <NanoTest/NanoTest.h>

// The two uniforms a clip becomes, and the one case that has to come out right
// without anybody drawing anything.
//
// Every shape in the interface reads a clip, clipped or not: the fragment stage
// multiplies by one region unconditionally, so that a clipped shape and an
// unclipped one share a pipeline and go out in the same instanced draw. Which
// makes the *absence* of a clip a value rather than a branch -- a region of no
// size, which maps every fragment to the middle of itself and reads a texel that
// is opaque. If that encoding is wrong the interface does not lose its clipping,
// it loses everything, and it does so on the first frame of anything that never
// asked to be clipped at all.

using namespace nano;
using namespace eacp;
using namespace eacp::UI;

namespace
{
// What the shader does with the two of them, in plain arithmetic: where in the
// clip's own box a fragment lands, and whether that is inside it.
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

    // Wherever the fragment is -- and these are the corners of a surface as well
    // as its middle -- it lands in the same place and that place is inside.
    for (auto point: {Point {0.f, 0.f}, Point {1920.f, 1080.f}, Point {-40.f, 12.f}})
    {
        check(isInside(placeOf(region, point)),
              "an unclipped fragment is never cut");
        check(placeOf(region, point).x == 0.f, "and reads one fixed texel");
    }

    // Which is the opaque one, since a zero-sized uv rect collapses to its own
    // origin however the place is interpolated.
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

    // And the place is what indexes the atlas rect, so the middle of the region
    // reads the middle of the mask.
    check(mask[0] + middle.x * mask[2] == 0.55f);
    check(mask[1] + middle.y * mask[3] == 0.275f);
};

auto tEmptyRegion = test("ClipMask/aRegionOfNoSizeIsNoClip") = []
{
    // Which is what a caller holding a clip it could not place has: the two are
    // one value, so nothing downstream has to test for both.
    check(ClipMask {}.isEmpty());
    check(ClipMask {{10.f, 10.f, 0.f, 5.f}, {}}.isEmpty(), "no width");
    check(!ClipMask {{10.f, 10.f, 5.f, 5.f}, {}}.isEmpty());
};
