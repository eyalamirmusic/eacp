#include "Common.h"

// The implicit capture a press takes, and the one way out of it.
//
// A mouse-down makes the view it landed in the target for every drag and the
// up that follow, wherever the pointer goes — which is what makes a slider
// keep tracking past its own edge. Opening a popup window from inside that
// press is the case with no up coming: the press now belongs to the menu, and
// without cancelMouseCapture the view underneath sits waiting for it forever,
// drawn pressed and hovered the whole time.

using namespace nano;
using namespace eacp::Graphics;

namespace
{
struct Recorder final : View
{
    Recorder() { setHandlesMouseEvents(true); }

    void mouseDown(const MouseEvent&) override { ++downs; }
    void mouseDragged(const MouseEvent&) override { ++drags; }
    void mouseUp(const MouseEvent&) override { ++ups; }
    void mouseEntered(const MouseEvent&) override { ++enters; }
    void mouseExited(const MouseEvent&) override { ++exits; }

    int downs = 0;
    int drags = 0;
    int ups = 0;
    int enters = 0;
    int exits = 0;
};

MouseEvent eventAt(Point position, MouseEventType type)
{
    auto event = MouseEvent {};
    event.pos = position;
    event.downPos = position;
    event.type = type;

    return event;
}
} // namespace

auto tCaptureFollowsThePressUntilCancelled =
    test("View/cancelMouseCaptureEndsTheDrag") = []
{
    auto root = View {};
    auto child = Recorder {};

    root.setBounds({0.f, 0.f, 200.f, 200.f});
    root.addSubview(child);
    child.setBounds({0.f, 0.f, 100.f, 100.f});

    root.dispatchMouseEvent(eventAt({10.f, 10.f}, MouseEventType::Down));
    check(child.downs == 1);

    // Captured: the pointer has left the view and the drag still arrives.
    root.dispatchMouseEvent(eventAt({150.f, 150.f}, MouseEventType::Dragged));
    check(child.drags == 1);

    root.cancelMouseCapture();

    root.dispatchMouseEvent(eventAt({150.f, 150.f}, MouseEventType::Dragged));
    root.dispatchMouseEvent(eventAt({150.f, 150.f}, MouseEventType::Up));

    check(child.drags == 1);
    check(child.ups == 0);
};

// And the hover goes with it: the pointer is over the popup now, so the view
// it left must be told, or it stays highlighted behind the menu.
auto tCancelMouseCaptureExitsTheHoveredView =
    test("View/cancelMouseCaptureExitsTheHoveredView") = []
{
    auto root = View {};
    auto child = Recorder {};

    root.setBounds({0.f, 0.f, 200.f, 200.f});
    root.addSubview(child);
    child.setBounds({0.f, 0.f, 100.f, 100.f});

    root.dispatchMouseEvent(eventAt({10.f, 10.f}, MouseEventType::Moved));
    check(child.enters == 1);
    check(child.exits == 0);

    root.cancelMouseCapture();
    check(child.exits == 1);

    // Nothing is hovered any more, so a second cancel says nothing twice.
    root.cancelMouseCapture();
    check(child.exits == 1);
};
