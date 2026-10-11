#include "Common.h"

#include <cmath>

using namespace nano;
using namespace eacp::Graphics;

namespace
{
struct MagnifyRecorder final : View
{
    MagnifyRecorder() { setHandlesMouseEvents(true); }

    void mouseMagnify(const MouseEvent& event) override
    {
        ++magnifies;
        last = event;
    }

    void mouseWheel(const MouseEvent&) override { ++wheels; }

    int magnifies = 0;
    int wheels = 0;
    MouseEvent last;
};

MouseEvent magnifyEvent(Point position, float scale)
{
    auto event = MouseEvent {};
    event.type = MouseEventType::Magnify;
    event.pos = position;
    event.downPos = position;
    event.magnifyScale = scale;
    return event;
}

bool near(float a, float b)
{
    return std::abs(a - b) < 1e-5f;
}
} // namespace

auto tMagnifyDefaults = test("Magnify/eventDefaultsToNoZoom") = []
{
    auto event = MouseEvent {};

    check(event.magnifyScale == 1.f);
};

auto tFromMagnification = test("Magnify/magnificationBecomesAScaleFactor") = []
{
    static_assert(magnifyScaleFromMagnification(0.0) == 1.f);

    check(near(magnifyScaleFromMagnification(0.25), 1.25f));
    check(near(magnifyScaleFromMagnification(-0.2), 0.8f));
    check(magnifyScaleFromMagnification(-1.0) == minimumMagnifyScale);
    check(magnifyScaleFromMagnification(-3.0) == minimumMagnifyScale,
          "never zero or negative");
};

auto tBetweenDistances = test("Magnify/theRatioOfTwoPinchDistancesIsTheScale") = []
{
    static_assert(magnifyScaleBetweenDistances(100.0, 100.0) == 1.f);

    check(near(magnifyScaleBetweenDistances(100.0, 125.0), 1.25f));
    check(near(magnifyScaleBetweenDistances(200.0, 150.0), 0.75f));
    check(magnifyScaleBetweenDistances(0.0, 150.0) == 1.f, "no previous step");
    check(magnifyScaleBetweenDistances(150.0, 0.0) == 1.f);
};

auto tReachesView = test("Magnify/reachesTheViewUnderThePointerInItsSpace") = []
{
    auto root = View {};
    auto child = MagnifyRecorder {};

    root.setBounds({0.f, 0.f, 200.f, 200.f});
    child.setBounds({50.f, 40.f, 100.f, 100.f});
    root.addSubview(child);

    auto event = magnifyEvent({70.f, 60.f}, 1.5f);
    event.scrollPhase = ScrollPhase::Began;
    event.modifiers.alt = true;
    root.dispatchMouseEvent(event);

    check(child.magnifies == 1);
    check(child.wheels == 0);
    check(child.last.type == MouseEventType::Magnify);
    check(child.last.magnifyScale == 1.5f);
    check(child.last.scrollPhase == ScrollPhase::Began);
    check(child.last.modifiers.alt);
    check(child.last.pos.x == 20.f);
    check(child.last.pos.y == 20.f);
};

auto tOutsideDropped = test("Magnify/outsideEveryViewIsNotDelivered") = []
{
    auto root = View {};
    auto child = MagnifyRecorder {};

    root.setBounds({0.f, 0.f, 200.f, 200.f});
    child.setBounds({0.f, 0.f, 20.f, 20.f});
    root.addSubview(child);

    root.dispatchMouseEvent(magnifyEvent({150.f, 150.f}, 1.2f));

    check(child.magnifies == 0);
};
