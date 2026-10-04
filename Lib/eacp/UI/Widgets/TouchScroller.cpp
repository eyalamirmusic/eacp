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
} // namespace

bool isWithinTouchSlop(Point from, Point to)
{
    return from.distanceTo(to) <= touchSlop;
}

bool isTouchTap(const MouseEvent& event)
{
    return event.fromTouch && isWithinTouchSlop(event.downPosition, event.position);
}

bool TouchScroller::isScrollMovement(Point movement)
{
    auto along = std::abs(movement.y);

    return along > touchSlop && along > std::abs(movement.x);
}

void TouchScroller::press(Point position, double time)
{
    stop();

    pressed = true;
    dragging = false;
    downPosition = position;
    lastY = position.y;

    sampleCount = 0;
    nextSample = 0;
    addSample(position.y, time);
}

float TouchScroller::drag(Point position, double time)
{
    if (!pressed)
        return 0.f;

    addSample(position.y, time);

    if (!dragging)
    {
        auto movement = position - downPosition;

        if (!isScrollMovement(movement))
            return 0.f;

        // From the edge of the slop rather than from where the finger went
        // down, so the content does not jump by the slop as the scroll starts.
        dragging = true;
        lastY = downPosition.y + std::copysign(touchSlop, movement.y);
    }

    auto offset = lastY - position.y;
    lastY = position.y;

    return offset;
}

bool TouchScroller::release(Point position, double time)
{
    if (!pressed)
        return false;

    addSample(position.y, time);

    pressed = false;

    auto wasDragging = dragging;
    dragging = false;

    if (!wasDragging)
        return false;

    auto speed = releaseVelocity(time);

    if (std::abs(speed) < minimumFlingVelocity)
        return false;

    velocity = std::clamp(speed, -maximumFlingVelocity, maximumFlingVelocity);

    return true;
}

float TouchScroller::advance(double seconds)
{
    if (velocity == 0.f || seconds <= 0.0)
        return 0.f;

    // Exactly, rather than a step at the start speed: the distance a frame
    // covers comes out the same whatever the frame rate.
    auto kept = std::exp(-flingDecayPerSecond * seconds);
    auto distance = velocity * (float) ((1.0 - kept) / flingDecayPerSecond);

    velocity *= (float) kept;

    if (std::abs(velocity) < stoppingVelocity)
        velocity = 0.f;

    return distance;
}

void TouchScroller::stop()
{
    velocity = 0.f;
}

bool TouchScroller::isDragging() const
{
    return dragging;
}

bool TouchScroller::isFlinging() const
{
    return velocity != 0.f;
}

float TouchScroller::getVelocity() const
{
    return velocity;
}

void TouchScroller::addSample(float y, double time)
{
    samples[nextSample] = {y, time};
    nextSample = (nextSample + 1) % samples.size();
    sampleCount = std::min(sampleCount + 1, samples.size());
}

float TouchScroller::releaseVelocity(double now) const
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
        return 0.f;

    return (float) ((oldest.y - last.y) / elapsed);
}

TouchScrolling::TouchScrolling(Component& ownerToUse)
    : owner(ownerToUse)
{
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
    auto wanted = offset + scroller.advance(seconds);
    auto clamped = std::clamp(wanted, 0.f, maximum);

    if (clamped != wanted)
        scroller.stop();

    return clamped;
}

bool TouchScrolling::isScrollDrag(const MouseEvent& event, float maximum)
{
    return maximum > 0.f
           && TouchScroller::isScrollMovement(event.position - event.downPosition);
}

bool TouchScrolling::isFlinging() const
{
    return scroller.isFlinging();
}
} // namespace eacp::UI
