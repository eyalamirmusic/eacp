#include "Common.h"

#include <eacp/Graphics/GlobalInput/GlobalMouseMonitor-Windows.h>

// What a global mouse monitor reports, from low-level hook input built here
// and handed to its translation: nothing is posted to the system.

using namespace nano;
using namespace eacp::Graphics;

namespace
{
constexpr auto pixelsPerPoint = 2.f;
constexpr auto pressX = 240;
constexpr auto pressY = 80;
constexpr auto dragX = 260;
constexpr auto dragY = 100;
constexpr auto oneDetentUp = static_cast<DWORD>(WHEEL_DELTA) << 16;

LowLevelMouseInput makeInput(WPARAM message, int x, int y, DWORD mouseData = 0)
{
    auto input = LowLevelMouseInput {};
    input.message = message;
    input.data.pt = {x, y};
    input.data.mouseData = mouseData;
    return input;
}
} // namespace

auto tPressIsInDesktopPoints =
    test("GlobalMouseMonitor/aPressIsInDesktopPoints") = []
{
    auto translator = LowLevelMouseTranslator {};
    auto event = translator.translate(makeInput(WM_RBUTTONDOWN, pressX, pressY),
                                      pixelsPerPoint);

    check(event.has_value());
    check(event->type == MouseEventType::Down);
    check(event->button == MouseButton::Right);
    check(event->pos.x == pressX / pixelsPerPoint);
    check(event->pos.y == pressY / pixelsPerPoint);
};

auto tMoveWithAButtonHeldIsADrag =
    test("GlobalMouseMonitor/aMoveWithAButtonHeldIsADrag") = []
{
    auto translator = LowLevelMouseTranslator {};
    translator.translate(makeInput(WM_MOUSEMOVE, pressX, pressY), pixelsPerPoint);
    translator.translate(makeInput(WM_LBUTTONDOWN, pressX, pressY), pixelsPerPoint);

    auto drag =
        translator.translate(makeInput(WM_MOUSEMOVE, dragX, dragY), pixelsPerPoint);

    check(drag.has_value());
    check(drag->type == MouseEventType::Dragged);
    check(drag->button == MouseButton::Left);
    check(drag->downPos.x == pressX / pixelsPerPoint);
    check(drag->delta.x == (dragX - pressX) / pixelsPerPoint);
    check(drag->delta.y == (dragY - pressY) / pixelsPerPoint);
};

auto tMoveAfterTheReleaseIsAMove =
    test("GlobalMouseMonitor/aMoveAfterTheReleaseIsAMove") = []
{
    auto translator = LowLevelMouseTranslator {};
    translator.translate(makeInput(WM_LBUTTONDOWN, pressX, pressY), pixelsPerPoint);
    translator.translate(makeInput(WM_LBUTTONUP, pressX, pressY), pixelsPerPoint);

    auto move =
        translator.translate(makeInput(WM_MOUSEMOVE, dragX, dragY), pixelsPerPoint);

    check(move.has_value());
    check(move->type == MouseEventType::Moved);
};

auto tWheelIsInLines = test("GlobalMouseMonitor/theWheelIsInLines") = []
{
    auto translator = LowLevelMouseTranslator {};
    auto wheel = translator.translate(
        makeInput(WM_MOUSEWHEEL, pressX, pressY, oneDetentUp), pixelsPerPoint);

    check(wheel.has_value());
    check(wheel->type == MouseEventType::Wheel);
    check(wheel->delta.x == 0.f);
    check(wheel->delta.y == 1.f);
};

auto tOtherMessagesAreNotMouseEvents =
    test("GlobalMouseMonitor/otherMessagesAreNotMouseEvents") = []
{
    auto translator = LowLevelMouseTranslator {};
    check(!translator.translate(makeInput(WM_KEYDOWN, 0, 0), pixelsPerPoint)
               .has_value());
};
