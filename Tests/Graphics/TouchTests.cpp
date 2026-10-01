#include "Common.h"

using namespace nano;
using namespace eacp::Graphics;

namespace
{
struct TouchRecorder final : View
{
    TouchRecorder() { setHandlesTouchEvents(true); }

    void touchBegan(const TouchEvent& event) override { events.add(event); }
    void touchMoved(const TouchEvent& event) override { events.add(event); }
    void touchEnded(const TouchEvent& event) override { events.add(event); }

    eacp::Vector<TouchEvent> events;
};

struct MouseRecorder final : View
{
    MouseRecorder() { setHandlesMouseEvents(true); }

    void mouseDown(const MouseEvent& event) override { events.add(event); }
    void mouseDragged(const MouseEvent& event) override { events.add(event); }
    void mouseUp(const MouseEvent& event) override { events.add(event); }

    eacp::Vector<MouseEvent> events;
};

struct InsetsRecorder final : View
{
    void safeAreaInsetsChanged() override { ++changes; }

    int changes = 0;
};

TouchEvent touch(int id, TouchPhase phase, Point position)
{
    auto event = TouchEvent {};
    event.id = id;
    event.phase = phase;
    event.pos = position;
    return event;
}

bool same(const Insets& a, const Insets& b)
{
    return a.top == b.top && a.left == b.left && a.bottom == b.bottom
           && a.right == b.right;
}
} // namespace

auto tTouchLocal = test("Touch/eachFingerReachesItsViewInLocalCoordinates") = []
{
    auto root = View {};
    auto pad = TouchRecorder {};

    root.setBounds({0.f, 0.f, 300.f, 300.f});
    pad.setBounds({100.f, 50.f, 100.f, 100.f});
    root.addSubview(pad);

    root.dispatchTouchEvent(touch(1, TouchPhase::Began, {120.f, 60.f}));
    root.dispatchTouchEvent(touch(1, TouchPhase::Moved, {150.f, 90.f}));
    root.dispatchTouchEvent(touch(1, TouchPhase::Ended, {150.f, 90.f}));

    check(pad.events.size() == 3);
    check(pad.events[0].phase == TouchPhase::Began);
    check(pad.events[0].pos.x == 20.f && pad.events[0].pos.y == 10.f);
    check(pad.events[1].pos.x == 50.f && pad.events[1].pos.y == 40.f);
    check(pad.events[1].downPos.x == 20.f && pad.events[1].downPos.y == 10.f);
    check(pad.events[2].phase == TouchPhase::Ended);
};

auto tTwoFingers = test("Touch/twoFingersAreTwoTouches") = []
{
    auto root = View {};
    auto pad = TouchRecorder {};

    root.setBounds({0.f, 0.f, 300.f, 300.f});
    pad.setBounds({0.f, 0.f, 300.f, 300.f});
    root.addSubview(pad);

    root.dispatchTouchEvent(touch(1, TouchPhase::Began, {10.f, 10.f}));
    root.dispatchTouchEvent(touch(2, TouchPhase::Began, {200.f, 200.f}));
    root.dispatchTouchEvent(touch(2, TouchPhase::Moved, {210.f, 190.f}));
    root.dispatchTouchEvent(touch(1, TouchPhase::Cancelled, {10.f, 10.f}));
    root.dispatchTouchEvent(touch(2, TouchPhase::Ended, {210.f, 190.f}));

    check(pad.events.size() == 5);
    check(pad.events[1].id == 2);
    check(pad.events[2].id == 2 && pad.events[2].downPos.x == 200.f);
    check(pad.events[3].id == 1 && pad.events[3].phase == TouchPhase::Cancelled);
    check(pad.events[4].id == 2 && pad.events[4].phase == TouchPhase::Ended);
};

auto tCaptured = test("Touch/fingerStaysWithTheViewItCameDownOn") = []
{
    auto root = View {};
    auto left = TouchRecorder {};
    auto right = TouchRecorder {};

    root.setBounds({0.f, 0.f, 200.f, 100.f});
    left.setBounds({0.f, 0.f, 100.f, 100.f});
    right.setBounds({100.f, 0.f, 100.f, 100.f});
    root.addChildren({left, right});

    root.dispatchTouchEvent(touch(7, TouchPhase::Began, {50.f, 50.f}));
    root.dispatchTouchEvent(touch(7, TouchPhase::Moved, {150.f, 50.f}));
    root.dispatchTouchEvent(touch(7, TouchPhase::Ended, {150.f, 50.f}));

    check(left.events.size() == 3);
    check(right.events.empty());
    check(left.events[1].pos.x == 150.f);
};

auto tMouseViews = test("Touch/drivesTheMouseOnViewsThatOnlyHandleIt") = []
{
    auto root = View {};
    auto button = MouseRecorder {};

    root.setBounds({0.f, 0.f, 200.f, 200.f});
    button.setBounds({50.f, 50.f, 100.f, 100.f});
    root.addSubview(button);

    root.dispatchTouchEvent(touch(1, TouchPhase::Began, {60.f, 70.f}));
    root.dispatchTouchEvent(touch(2, TouchPhase::Began, {100.f, 100.f}));
    root.dispatchTouchEvent(touch(1, TouchPhase::Moved, {80.f, 90.f}));
    root.dispatchTouchEvent(touch(2, TouchPhase::Ended, {100.f, 100.f}));
    root.dispatchTouchEvent(touch(1, TouchPhase::Ended, {80.f, 90.f}));

    check(button.events.size() == 3);
    check(button.events[0].type == MouseEventType::Down);
    check(button.events[0].pos.x == 10.f && button.events[0].pos.y == 20.f);
    check(button.events[1].type == MouseEventType::Dragged);
    check(button.events[1].downPos.x == 10.f);
    check(button.events[2].type == MouseEventType::Up);
};

auto tNoDoubleDelivery = test("Touch/aTouchViewGetsNoMouseEvents") = []
{
    struct Both final : View
    {
        Both() { setHandlesTouchEvents(true).setHandlesMouseEvents(true); }

        void touchBegan(const TouchEvent&) override { ++touches; }
        void mouseDown(const MouseEvent&) override { ++mice; }

        int touches = 0;
        int mice = 0;
    };

    auto root = View {};
    auto both = Both {};

    root.setBounds({0.f, 0.f, 100.f, 100.f});
    both.setBounds({0.f, 0.f, 100.f, 100.f});
    root.addSubview(both);

    root.dispatchTouchEvent(touch(1, TouchPhase::Began, {10.f, 10.f}));

    check(both.touches == 1);
    check(both.mice == 0);
};

auto tRemoved = test("Touch/removedViewHearsNoMore") = []
{
    auto root = View {};
    auto holder = View {};
    auto pad = TouchRecorder {};

    root.setBounds({0.f, 0.f, 100.f, 100.f});
    holder.setBounds({0.f, 0.f, 100.f, 100.f});
    pad.setBounds({0.f, 0.f, 100.f, 100.f});
    holder.addSubview(pad);
    root.addSubview(holder);

    root.dispatchTouchEvent(touch(1, TouchPhase::Began, {10.f, 10.f}));
    root.removeSubview(holder);
    root.dispatchTouchEvent(touch(1, TouchPhase::Moved, {20.f, 20.f}));

    check(pad.events.size() == 1);
};

auto tSafeAreaZero = test("SafeArea/zeroUntilThePlatformSaysOtherwise") = []
{
    auto root = View {};

    check(same(root.getSafeAreaInsets(), {}));
};

auto tSafeAreaChild = test("SafeArea/childSeesTheOverlapWithItsBounds") = []
{
    auto root = View {};
    auto full = View {};
    auto middle = View {};

    root.setBounds({0.f, 0.f, 400.f, 800.f});
    full.setBounds({0.f, 0.f, 400.f, 800.f});
    middle.setBounds({0.f, 100.f, 400.f, 600.f});
    root.addChildren({full, middle});

    root.setSafeAreaInsets({.top = 59.f, .left = 0.f, .bottom = 34.f, .right = 0.f});

    check(same(full.getSafeAreaInsets(), {.top = 59.f, .bottom = 34.f}));
    check(same(middle.getSafeAreaInsets(), {}));

    middle.setBounds({10.f, 40.f, 380.f, 740.f});
    check(same(middle.getSafeAreaInsets(), {.top = 19.f, .bottom = 14.f}));
};

auto tSafeAreaNotify = test("SafeArea/everyViewHearsAChangeOnce") = []
{
    auto root = InsetsRecorder {};
    auto child = InsetsRecorder {};
    auto grandchild = InsetsRecorder {};

    child.addSubview(grandchild);
    root.addSubview(child);

    root.setSafeAreaInsets({.bottom = 34.f});
    root.setSafeAreaInsets({.bottom = 34.f});

    check(root.changes == 1);
    check(child.changes == 1);
    check(grandchild.changes == 1);
};
