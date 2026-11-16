#include "PointerTrackers.h"

#include <cmath>

namespace eacp::Graphics
{
namespace
{
// No seat event carries a click count, so this is the framework's own figure:
// 400ms is what X11, GTK and Qt all default to.
constexpr uint32_t pointerDoubleClickIntervalMs = 400;
constexpr float pointerDoubleClickSlopPoints = 5.f;
} // namespace

int PointerTracker::pressed(MouseButton button, uint32_t timeMilliseconds)
{
    auto near =
        std::abs(position.x - lastClickPosition.x) <= pointerDoubleClickSlopPoints
        && std::abs(position.y - lastClickPosition.y)
               <= pointerDoubleClickSlopPoints;
    auto soon = timeMilliseconds - lastClickTime <= pointerDoubleClickIntervalMs;

    clickCount = (near && soon && button == lastClickButton) ? clickCount + 1 : 1;
    lastClickTime = timeMilliseconds;
    lastClickButton = button;
    lastClickPosition = position;

    buttonHeld = true;
    heldButton = button;
    downPosition = position;

    return clickCount;
}

void WheelTracker::addDelta(Point delta)
{
    this->delta.x += delta.x;
    this->delta.y += delta.y;

    pending = true;
}

void WheelTracker::addNotches(Point notches)
{
    this->notches.x += notches.x;
    this->notches.y += notches.y;

    hasNotches = true;
    pending = true;
}

void WheelTracker::setSource(bool isPrecise, bool isGesture)
{
    precise = isPrecise;
    gesture = isGesture;
}

void WheelTracker::setStopped()
{
    stopped = true;
    pending = true;
}

Point WheelTracker::getDelta() const
{
    return (!precise && hasNotches) ? notches : delta;
}

void WheelTracker::endFrame()
{
    delta = {};
    notches = {};
    hasNotches = false;
    pending = false;
    stopped = false;
}

bool CursorTracker::setShape(MouseCursor::Shape newShape)
{
    if (newShape == shape && !hidden)
        return false;

    shape = newShape;
    hidden = false;

    return true;
}

bool CursorTracker::setHidden(bool shouldHide)
{
    if (hidden == shouldHide)
        return false;

    hidden = shouldHide;

    return true;
}
} // namespace eacp::Graphics
