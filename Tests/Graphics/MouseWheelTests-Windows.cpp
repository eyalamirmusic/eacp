#include "Common.h"

#include <eacp/Core/Utils/WinInclude.h>

// A notched wheel reports lines on every platform - a detent is one - and the
// scrolling widgets multiply by what a line is worth to them. Windows used to
// pass WHEEL_DELTA units through instead, so a detent was 120 lines there: a
// ScrollPanel jumped 4800 points, and Cows In Love's zoom hit its limit.

using namespace nano;
using namespace eacp;
using namespace eacp::Graphics;

namespace
{
struct Recorder final : View
{
    Recorder() { setHandlesMouseEvents(true); }

    void mouseWheel(const MouseEvent& event) override { events.add(event); }

    Vector<MouseEvent> events;
};

void turnWheel(HWND hwnd, UINT message, int wheelDelta)
{
    auto center = POINT {40, 40};
    ClientToScreen(hwnd, &center);
    SendMessageW(hwnd,
                 message,
                 MAKEWPARAM(0, static_cast<WORD>(static_cast<short>(wheelDelta))),
                 MAKELPARAM(center.x, center.y));
}
} // namespace

auto tWheelReportsLines = test("Window/wheelReportsLines") = []
{
    auto window = Window {};
    auto content = Recorder {};
    window.setContentView(content);
    auto hwnd = static_cast<HWND>(window.getHandle());

    turnWheel(hwnd, WM_MOUSEWHEEL, WHEEL_DELTA);
    turnWheel(hwnd, WM_MOUSEWHEEL, -2 * WHEEL_DELTA);
    turnWheel(hwnd, WM_MOUSEWHEEL, WHEEL_DELTA / 4);
    turnWheel(hwnd, WM_MOUSEHWHEEL, WHEEL_DELTA);

    auto& events = content.events;
    check(events.size() == 4);

    if (events.size() != 4)
        return;

    check(events[0].delta.y == 1.f && events[0].delta.x == 0.f);
    check(events[1].delta.y == -2.f);
    check(events[2].delta.y == 0.25f);
    check(events[3].delta.x == 1.f && events[3].delta.y == 0.f);

    for (const auto& event: events)
        check(!event.preciseScrolling);
};
