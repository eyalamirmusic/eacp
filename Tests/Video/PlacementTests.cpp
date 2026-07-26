#include <eacp/VideoView/VideoView.h>
#include <NanoTest/NanoTest.h>

#include <cmath>

using namespace nano;
using namespace eacp;
using View = Video::VideoView;

namespace
{
bool pointEquals(const Graphics::Point& point, float x, float y)
{
    return std::abs(point.x - x) < 0.001f && std::abs(point.y - y) < 0.001f;
}

// Where the texture's four corners land on screen under a placement, which is
// what actually matters: (0,0) is the texture's top-left, (1,1) its
// bottom-right.
Graphics::Point corner(const View::Placement& placement, float u, float v)
{
    return {placement.origin.x + u * placement.edgeX.x + v * placement.edgeY.x,
            placement.origin.y + u * placement.edgeX.y + v * placement.edgeY.y};
}

const auto area = Graphics::Rect {10.0f, 20.0f, 100.0f, 40.0f};
} // namespace

// No rotation: the texture maps straight onto the rect.
auto tUnrotated = test("VideoView/placementUnrotated") = []
{
    auto placement = View::computePlacement(area, 0, false);

    check(pointEquals(corner(placement, 0, 0), 10, 20)); // top-left
    check(pointEquals(corner(placement, 1, 0), 110, 20)); // top-right
    check(pointEquals(corner(placement, 1, 1), 110, 60)); // bottom-right
};

// A quarter turn clockwise: the texture's top-left goes to the rect's
// top-right, and its u axis runs down the screen — the orientation no
// combination of flips can produce, which is why this exists.
auto tRotated90 = test("VideoView/placement90") = []
{
    auto placement = View::computePlacement(area, 90, false);

    check(pointEquals(corner(placement, 0, 0), 110, 20)); // top-left -> top-right
    check(pointEquals(corner(placement, 1, 0), 110, 60)); // u runs downwards
    check(pointEquals(corner(placement, 0, 1), 10, 20)); // v runs leftwards
};

// A half turn puts the texture's top-left at the rect's bottom-right.
auto tRotated180 = test("VideoView/placement180") = []
{
    auto placement = View::computePlacement(area, 180, false);

    check(pointEquals(corner(placement, 0, 0), 110, 60));
    check(pointEquals(corner(placement, 1, 1), 10, 20));
};

// Three quarter turns clockwise, i.e. a quarter turn the other way.
auto tRotated270 = test("VideoView/placement270") = []
{
    auto placement = View::computePlacement(area, 270, false);

    check(pointEquals(corner(placement, 0, 0), 10, 60)); // top-left -> bottom-left
    check(pointEquals(corner(placement, 1, 0), 10, 20)); // u runs upwards
    check(pointEquals(corner(placement, 0, 1), 110, 60)); // v runs rightwards
};

// Mirroring flips the *displayed* image horizontally, so on an unrotated frame
// the texture's left edge lands on the right of the rect.
auto tMirrored = test("VideoView/placementMirrored") = []
{
    auto placement = View::computePlacement(area, 0, true);

    check(pointEquals(corner(placement, 0, 0), 110, 20));
    check(pointEquals(corner(placement, 1, 0), 10, 20));
    check(pointEquals(corner(placement, 1, 1), 10, 60));
};

// Mirror composed with a rotation stays a horizontal flip of what is shown, not
// of the stored texture: at 90 degrees the mirrored image runs up the screen
// where the unmirrored one ran down.
auto tMirroredRotated = test("VideoView/placementMirrored90") = []
{
    auto placement = View::computePlacement(area, 90, true);

    check(pointEquals(corner(placement, 0, 0), 110, 60));
    check(pointEquals(corner(placement, 1, 0), 110, 20));
};

// The rotation is normalised, so a track reporting a negative or over-wound
// angle still lands on one of the four quarter turns.
auto tNormalisesAngle = test("VideoView/placementNormalisesAngle") = []
{
    auto wound = View::computePlacement(area, 450, false);
    auto plain = View::computePlacement(area, 90, false);

    check(pointEquals(wound.origin, plain.origin.x, plain.origin.y));
    check(pointEquals(wound.edgeX, plain.edgeX.x, plain.edgeX.y));

    auto negative = View::computePlacement(area, -90, false);
    auto same = View::computePlacement(area, 270, false);

    check(pointEquals(negative.origin, same.origin.x, same.origin.y));
};

// A quarter-turned track is stored landscape and shown portrait, so the fit has
// to be computed against the swapped size or a phone clip letterboxes wrongly.
auto tDisplaySize = test("VideoView/displaySizeSwapsOnQuarterTurn") = []
{
    check(pointEquals(View::displaySize(1920, 1080, 0), 1920, 1080));
    check(pointEquals(View::displaySize(1920, 1080, 180), 1920, 1080));
    check(pointEquals(View::displaySize(1920, 1080, 90), 1080, 1920));
    check(pointEquals(View::displaySize(1920, 1080, 270), 1080, 1920));
};
