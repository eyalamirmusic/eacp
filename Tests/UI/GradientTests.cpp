#include <eacp/UI/UI.h>

#include <NanoTest/NanoTest.h>

#include <cmath>

using namespace nano;
using namespace eacp;
using namespace eacp::UI;

namespace
{
const auto red = Color {1.f, 0.f, 0.f, 1.f};
const auto blue = Color {0.f, 0.f, 1.f, 1.f};
const auto green = Color {0.f, 1.f, 0.f, 1.f};

bool same(const Color& a, const Color& b)
{
    return a.r == b.r && a.g == b.g && a.b == b.b && a.a == b.a;
}

bool same(const Point& a, const Point& b)
{
    return a.x == b.x && a.y == b.y;
}

bool hasTwoStops(const Gradient& gradient, const Color& from, const Color& to)
{
    return gradient.stops.size() == 2 && same(gradient.stops[0].color, from)
           && gradient.stops[0].position == 0.f && same(gradient.stops[1].color, to)
           && gradient.stops[1].position == 1.f;
}
} // namespace

auto tLinear = test("Gradient/linearRunsFromTheFirstColourToTheSecond") = []
{
    auto gradient = Gradient::linear({1.f, 2.f}, red, {3.f, 4.f}, blue);

    check(gradient.kind == Gradient::Kind::Linear);
    check(same(gradient.start, {1.f, 2.f}));
    check(same(gradient.end, {3.f, 4.f}));
    check(hasTwoStops(gradient, red, blue));
    check(!gradient.isEmpty());
};

auto tRadial = test("Gradient/radialReachesTheEdgeColourAtTheEdgePoint") = []
{
    auto gradient = Gradient::radial({10.f, 10.f}, red, {13.f, 14.f}, blue);

    check(gradient.kind == Gradient::Kind::Radial);
    check(same(gradient.start, {10.f, 10.f}));
    check(std::abs(gradient.radius - 5.f) < 0.0001f);
    check(hasTwoStops(gradient, red, blue));
};

auto tVertical = test("Gradient/verticalRunsDownTheYAxis") = []
{
    auto byCoordinate = Gradient::vertical(red, 5.f, blue, 25.f);

    check(same(byCoordinate.start, {0.f, 5.f}));
    check(same(byCoordinate.end, {0.f, 25.f}));
    check(hasTwoStops(byCoordinate, red, blue));

    auto byArea = Gradient::vertical(red, blue, {3.f, 5.f, 40.f, 20.f});

    check(byArea.start.y == 5.f);
    check(byArea.end.y == 25.f);
    check(byArea.start.x == byArea.end.x);
};

auto tHorizontal = test("Gradient/horizontalRunsAlongTheXAxis") = []
{
    auto byCoordinate = Gradient::horizontal(red, 10.f, blue, 2.f);

    check(same(byCoordinate.start, {10.f, 0.f}));
    check(same(byCoordinate.end, {2.f, 0.f}));
    check(hasTwoStops(byCoordinate, red, blue));

    auto byArea = Gradient::horizontal(red, blue, {3.f, 5.f, 40.f, 20.f});

    check(byArea.start.x == 3.f);
    check(byArea.end.x == 43.f);
    check(byArea.start.y == byArea.end.y);
};

auto tAddStop = test("Gradient/addStopAppendsAndChains") = []
{
    auto gradient = Gradient::vertical(red, 0.f, blue, 1.f);
    gradient.addStop(0.5f, green).addStop(0.25f, red);

    check(gradient.stops.size() == 4);
    check(gradient.stops[2].position == 0.5f);
    check(same(gradient.stops[2].color, green));
    check(gradient.stops[3].position == 0.25f);
};

auto tEmpty = test("Gradient/aDefaultGradientIsEmpty") = []
{
    check(Gradient {}.isEmpty());
};
