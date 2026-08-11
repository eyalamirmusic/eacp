#include "Common.h"

using namespace nano;
using namespace eacp::Graphics;

namespace
{
struct WheelRecorder final : View
{
    WheelRecorder() { setHandlesMouseEvents(true); }

    void mouseWheel(const MouseEvent& event) override
    {
        ++wheels;
        last = event;
    }

    int wheels = 0;
    MouseEvent last;
};

MouseEvent wheelEvent(Point position, Point delta)
{
    auto event = MouseEvent {};
    event.type = MouseEventType::Wheel;
    event.pos = position;
    event.delta = delta;
    event.rawDelta = delta;
    return event;
}
} // namespace

auto tWheelDefaults = test("ScrollWheel/eventDefaultsToNonPreciseNoPhase") = []
{
    auto event = MouseEvent {};

    check(!event.preciseScrolling);
    check(event.scrollPhase == ScrollPhase::None);
};

auto tWheelReachesView = test("ScrollWheel/reachesTheViewUnderTheCursor") = []
{
    auto root = View {};
    auto child = WheelRecorder {};

    root.setBounds({0.f, 0.f, 200.f, 200.f});
    child.setBounds({50.f, 50.f, 100.f, 100.f});
    root.addSubview(child);

    root.dispatchMouseEvent(wheelEvent({100.f, 100.f}, {0.f, -12.f}));

    check(child.wheels == 1);
    check(child.last.type == MouseEventType::Wheel);
    check(child.last.delta.y == -12.f);
};

auto tWheelPositionIsLocal = test("ScrollWheel/positionIsViewLocal") = []
{
    auto root = View {};
    auto child = WheelRecorder {};

    root.setBounds({0.f, 0.f, 200.f, 200.f});
    child.setBounds({50.f, 40.f, 100.f, 100.f});
    root.addSubview(child);

    root.dispatchMouseEvent(wheelEvent({70.f, 60.f}, {0.f, 3.f}));

    check(child.wheels == 1);
    check(child.last.pos.x == 20.f);
    check(child.last.pos.y == 20.f);
};

auto tWheelOutsideIsDropped = test("ScrollWheel/outsideChildIsNotDelivered") = []
{
    auto root = View {};
    auto child = WheelRecorder {};

    root.setBounds({0.f, 0.f, 200.f, 200.f});
    child.setBounds({0.f, 0.f, 20.f, 20.f});
    root.addSubview(child);

    root.dispatchMouseEvent(wheelEvent({150.f, 150.f}, {0.f, -5.f}));

    check(child.wheels == 0);
};

auto tWheelPreservesScrollFields =
    test("ScrollWheel/routingPreservesScrollFields") = []
{
    auto root = View {};
    auto child = WheelRecorder {};

    root.setBounds({0.f, 0.f, 200.f, 200.f});
    child.setBounds({0.f, 0.f, 200.f, 200.f});
    root.addSubview(child);

    auto event = wheelEvent({100.f, 100.f}, {0.f, -8.f});
    event.preciseScrolling = true;
    event.scrollPhase = ScrollPhase::Momentum;
    event.modifiers.command = true;

    root.dispatchMouseEvent(event);

    check(child.wheels == 1);
    check(child.last.preciseScrolling);
    check(child.last.scrollPhase == ScrollPhase::Momentum);
    check(child.last.modifiers.command);
};

auto tWheelIgnoresMouseCapture = test("ScrollWheel/ignoresMouseDownCapture") = []
{
    auto root = View {};
    auto first = WheelRecorder {};
    auto second = WheelRecorder {};

    root.setBounds({0.f, 0.f, 200.f, 200.f});
    first.setBounds({0.f, 0.f, 100.f, 200.f});
    second.setBounds({100.f, 0.f, 100.f, 200.f});
    root.addSubview(first);
    root.addSubview(second);

    auto down = MouseEvent {};
    down.type = MouseEventType::Down;
    down.pos = {50.f, 100.f};
    root.dispatchMouseEvent(down);

    root.dispatchMouseEvent(wheelEvent({150.f, 100.f}, {0.f, -4.f}));

    check(first.wheels == 0);
    check(second.wheels == 1);
};
