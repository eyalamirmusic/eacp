#include "TouchScroller.h"

#include "../Component/Component.h"

#include <algorithm>
#include <cmath>

namespace eacp::UI
{
namespace
{
// Only the last tenth of a second of the drag decides the fling, so a finger
// that stopped and then lifted flings nothing.
constexpr auto flingSampleWindow = 0.1;

constexpr auto minimumFlingVelocity = 50.f;
constexpr auto maximumFlingVelocity = 8000.f;

// Below this a fling is a crawl nobody can see, and it stops.
constexpr auto stoppingVelocity = 10.f;

// UIScrollView's normal deceleration, 0.998 of the speed kept per millisecond:
// a fling travels about half a second's worth of its starting speed in all.
constexpr auto flingDecayPerSecond = 2.002;

Point scaled(Point point, float factor)
{
    return {point.x * factor, point.y * factor};
}

// Exact for a point on one axis, where hypot answers that component's size and
// not a rounding of it, so a vertical scroller measures what it always did.
float magnitude(Point point)
{
    return std::hypot(point.x, point.y);
}
} // namespace

bool isWithinTouchSlop(Point from, Point to)
{
    return from.distanceTo(to) <= touchSlop;
}

bool isTouchTap(const MouseEvent& event)
{
    return event.fromTouch && isWithinTouchSlop(event.downPosition, event.position);
}

TouchScroller::TouchScroller(ScrollAxes axesToUse)
    : axes(axesToUse)
{
}

void TouchScroller::setAxes(ScrollAxes newAxes)
{
    axes = newAxes;
}

ScrollAxes TouchScroller::getAxes() const
{
    return axes;
}

bool TouchScroller::isScrollMovement(Point movement, ScrollAxes axes)
{
    auto across = std::abs(movement.x);
    auto upAndDown = std::abs(movement.y);

    switch (axes)
    {
        case ScrollAxes::vertical:
            return upAndDown > touchSlop && upAndDown > across;
        case ScrollAxes::horizontal:
            return across > touchSlop && across > upAndDown;
        case ScrollAxes::both:
            break;
    }

    return magnitude(movement) > touchSlop;
}

Point TouchScroller::alongAxes(Point point) const
{
    switch (axes)
    {
        case ScrollAxes::vertical:
            return {0.f, point.y};
        case ScrollAxes::horizontal:
            return {point.x, 0.f};
        case ScrollAxes::both:
            break;
    }

    return point;
}

Point TouchScroller::slopEdge(Point movement) const
{
    switch (axes)
    {
        case ScrollAxes::vertical:
            return {0.f, std::copysign(touchSlop, movement.y)};
        case ScrollAxes::horizontal:
            return {std::copysign(touchSlop, movement.x), 0.f};
        case ScrollAxes::both:
            break;
    }

    return scaled(movement.normalized(), touchSlop);
}

void TouchScroller::press(Point position, double time)
{
    stop();

    pressed = true;
    dragging = false;
    downPosition = position;
    lastPosition = alongAxes(position);

    sampleCount = 0;
    nextSample = 0;
    addSample(position, time);
}

Point TouchScroller::drag(Point position, double time)
{
    if (!pressed)
        return {};

    addSample(position, time);

    if (!dragging)
    {
        auto movement = position - downPosition;

        if (!isScrollMovement(movement, axes))
            return {};

        // From the edge of the slop rather than from where the finger went
        // down, so the content does not jump by the slop as the scroll starts.
        dragging = true;
        lastPosition = alongAxes(downPosition) + slopEdge(movement);
    }

    auto current = alongAxes(position);
    auto offset = lastPosition - current;
    lastPosition = current;

    return offset;
}

bool TouchScroller::release(Point position, double time)
{
    if (!pressed)
        return false;

    addSample(position, time);

    pressed = false;

    auto wasDragging = dragging;
    dragging = false;

    if (!wasDragging)
        return false;

    auto speed = releaseVelocity(time);
    auto size = magnitude(speed);

    if (size < minimumFlingVelocity)
        return false;

    velocity = speed;

    if (size > maximumFlingVelocity)
        velocity = scaled(speed, maximumFlingVelocity / size);

    return true;
}

Point TouchScroller::advance(double seconds)
{
    if (!isFlinging() || seconds <= 0.0)
        return {};

    // Exactly, rather than a step at the start speed: the distance a frame
    // covers comes out the same whatever the frame rate.
    auto kept = std::exp(-flingDecayPerSecond * seconds);
    auto distance = scaled(velocity, (float) ((1.0 - kept) / flingDecayPerSecond));

    velocity = scaled(velocity, (float) kept);

    if (magnitude(velocity) < stoppingVelocity)
        velocity = {};

    return distance;
}

void TouchScroller::stop()
{
    velocity = {};
}

void TouchScroller::stopHorizontally()
{
    velocity.x = 0.f;
}

void TouchScroller::stopVertically()
{
    velocity.y = 0.f;
}

bool TouchScroller::isDragging() const
{
    return dragging;
}

bool TouchScroller::isFlinging() const
{
    return velocity.x != 0.f || velocity.y != 0.f;
}

Point TouchScroller::getVelocity() const
{
    return velocity;
}

void TouchScroller::addSample(Point position, double time)
{
    samples[nextSample] = {alongAxes(position), time};
    nextSample = (nextSample + 1) % samples.size();
    sampleCount = std::min(sampleCount + 1, samples.size());
}

Point TouchScroller::releaseVelocity(double now) const
{
    auto newest = (nextSample + samples.size() - 1) % samples.size();
    const auto& last = samples[newest];
    auto oldest = last;

    for (auto age = 1; age < sampleCount; ++age)
    {
        auto index = (newest - age + samples.size()) % samples.size();
        const auto& sample = samples[index];

        if (now - sample.time > flingSampleWindow)
            break;

        oldest = sample;
    }

    auto elapsed = last.time - oldest.time;

    if (elapsed <= 0.0)
        return {};

    auto travelled = oldest.position - last.position;

    return {(float) (travelled.x / elapsed), (float) (travelled.y / elapsed)};
}

TouchScrolling::TouchScrolling(Component& ownerToUse, ScrollAxes axes)
    : owner(ownerToUse)
    , scroller(axes)
{
}

void TouchScrolling::setAxes(ScrollAxes newAxes)
{
    scroller.setAxes(newAxes);
}

void TouchScrolling::press(const MouseEvent& event)
{
    auto wasFlinging = isFlinging();

    stop();
    scroller.press(event.position, event.timestamp);
    pressStoppedFling = wasFlinging;
}

float TouchScrolling::drag(const MouseEvent& event, float offset)
{
    return drag(event, Point {0.f, offset}).y;
}

Point TouchScrolling::drag(const MouseEvent& event, Point offset)
{
    return offset + scroller.drag(event.position, event.timestamp);
}

bool TouchScrolling::release(const MouseEvent& event)
{
    auto wasTap = !scroller.isDragging() && !pressStoppedFling;
    pressStoppedFling = false;

    if (scroller.release(event.position, event.timestamp))
        owner.startAnimating();

    return wasTap;
}

void TouchScrolling::stop()
{
    pressStoppedFling = false;
    scroller.stop();
    owner.stopAnimating();
}

float TouchScrolling::advance(double seconds, float offset, float maximum)
{
    return advance(seconds, Point {0.f, offset}, Point {0.f, maximum}).y;
}

Point TouchScrolling::advance(double seconds, Point offset, Point maximum)
{
    auto wanted = offset + scroller.advance(seconds);
    auto clamped = Point {std::clamp(wanted.x, 0.f, std::max(0.f, maximum.x)),
                          std::clamp(wanted.y, 0.f, std::max(0.f, maximum.y))};

    if (clamped.x != wanted.x)
        scroller.stopHorizontally();

    if (clamped.y != wanted.y)
        scroller.stopVertically();

    return clamped;
}

bool TouchScrolling::isScrollDrag(const MouseEvent& event, float maximum)
{
    return isScrollDrag(event, Point {0.f, maximum});
}

bool TouchScrolling::isScrollDrag(const MouseEvent& event, Point maximum)
{
    if (maximum.x <= 0.f && maximum.y <= 0.f)
        return false;

    return TouchScroller::isScrollMovement(event.position - event.downPosition,
                                           axesWithRoom(maximum));
}

ScrollAxes TouchScrolling::axesWithRoom(Point maximum)
{
    auto across = maximum.x > 0.f;
    auto upAndDown = maximum.y > 0.f;

    if (across && upAndDown)
        return ScrollAxes::both;

    if (across)
        return ScrollAxes::horizontal;

    return ScrollAxes::vertical;
}

bool TouchScrolling::isFlinging() const
{
    return scroller.isFlinging();
}
} // namespace eacp::UI
