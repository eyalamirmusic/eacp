#include "Common.h"

#include <eacp/Core/Utils/WinInclude.h>

// Windows reports where the pointer is, never how far it moved, so the host
// derives MouseEvent::delta from the previous position as the Wayland and X11
// backends do. Before it did, every drag reported no movement at all, and a
// camera orbited by the pointer's delta (Cows In Love) stood still on Windows.

using namespace nano;
using namespace eacp;
using namespace eacp::Graphics;

namespace
{
struct Recorder final : View
{
    Recorder() { setHandlesMouseEvents(true); }

    void mouseDown(const MouseEvent& event) override { events.add(event); }
    void mouseDragged(const MouseEvent& event) override { events.add(event); }
    void mouseUp(const MouseEvent& event) override { events.add(event); }
    void mouseMoved(const MouseEvent& event) override { events.add(event); }

    Vector<MouseEvent> events;
};

void send(HWND hwnd, UINT message, WPARAM buttons, int x, int y)
{
    SendMessageW(hwnd, message, buttons, MAKELPARAM(x, y));
}

bool movedBy(const MouseEvent& event, const MouseEvent& previous)
{
    return event.delta.x == event.pos.x - previous.pos.x
           && event.delta.y == event.pos.y - previous.pos.y;
}
} // namespace

auto tDragReportsThePointersMovement =
    test("Window/dragReportsThePointersMovement") = []
{
    auto window = Window {};
    auto content = Recorder {};
    window.setContentView(content);
    auto hwnd = static_cast<HWND>(window.getHandle());

    send(hwnd, WM_LBUTTONDOWN, MK_LBUTTON, 100, 100);
    send(hwnd, WM_MOUSEMOVE, MK_LBUTTON, 130, 120);
    send(hwnd, WM_MOUSEMOVE, MK_LBUTTON, 140, 110);
    send(hwnd, WM_LBUTTONUP, 0, 140, 110);
    send(hwnd, WM_MOUSEMOVE, 0, 150, 130);

    auto& events = content.events;
    check(events.size() == 5);

    if (events.size() != 5)
        return;

    check(events[0].type == MouseEventType::Down);
    check(events[1].type == MouseEventType::Dragged);
    check(events[2].type == MouseEventType::Dragged);
    check(events[3].type == MouseEventType::Up);
    check(events[4].type == MouseEventType::Moved);

    check(events[1].pos.x > events[0].pos.x && events[1].pos.y > events[0].pos.y);
    check(movedBy(events[1], events[0]));
    check(movedBy(events[2], events[1]));
    check(movedBy(events[4], events[3]));
};
