#pragma once

#include "../Component/MouseEvent.h"

namespace eacp::UI
{
class Component;

// How far a finger travels, in points, before a press becomes a drag. Android's
// own figure: below it a finger resting on the glass still wanders, and a tap
// that moved a pixel or two is still a tap.
constexpr auto touchSlop = 8.f;

// Whether a finger that went down at `from` and is now at `to` is still a tap.
bool isWithinTouchSlop(Point from, Point to);

// A touch event whose finger has not left the slop around where it went down.
bool isTouchTap(const MouseEvent& event);

// A vertical scroll driven by a finger: the content follows the finger once it
// has moved past the slop, and carries on at the speed it was let go with,
// slowing exponentially until it stops.
//
// It owns no position. Every call answers how far the scroll offset should move
// -- positive toward the end of the content, the opposite way to the finger --
// and the component that owns the offset clamps it. Hitting an edge is the
// owner's to notice and answer with stop(), since only it knows where the edges
// are.
//
// Time is whatever the caller passes, never read from a clock, so the same
// gesture always produces the same scroll.
class TouchScroller
{
public:
    // Whether a finger that has moved this far from where it went down is
    // scrolling something vertical: past the slop, and more up-and-down than
    // across.
    static bool isScrollMovement(Point movement);

    // A finger went down. Stops a fling, which is what a press during one means.
    void press(Point position, double time);

    // The finger moved. Zero until the movement is a scroll; from then on every
    // move counts, across or not, so a scroll that has started is not lost to a
    // finger drifting sideways.
    float drag(Point position, double time);

    // The finger lifted. Starts a fling when it was still moving fast enough,
    // measured over the last moments of the drag, and answers whether it did.
    bool release(Point position, double time);

    // How far a fling carries the scroll in `seconds`, slowing it as it goes.
    // Zero once it has stopped.
    float advance(double seconds);

    void stop();

    bool isDragging() const { return dragging; }
    bool isFlinging() const { return velocity != 0.f; }

    // Scroll offset per second, positive toward the end of the content.
    float getVelocity() const { return velocity; }

private:
    struct Sample
    {
        float y = 0.f;
        double time = 0.0;
    };

    void addSample(float y, double time);
    float releaseVelocity(double now) const;

    Array<Sample, 8> samples;
    int sampleCount = 0;
    int nextSample = 0;

    Point downPosition;
    float lastY = 0.f;
    float velocity = 0.f;
    bool pressed = false;
    bool dragging = false;
};

// A TouchScroller wired to the component that scrolls: what ScrollPanel and
// ListBox share, so each keeps only what a tap means to it. Touch events only;
// a mouse is the owner's business.
//
// Every offset passed in is the owner's current one and every offset answered
// is where it should move, which the owner clamps as it always does. The fling
// runs on the owner's animation (Component::startAnimating), and the owner's
// advanceAnimation forwards to advance().
class TouchScrolling
{
public:
    explicit TouchScrolling(Component& ownerToUse);

    void press(const MouseEvent& event);
    float drag(const MouseEvent& event, float offset);

    // Starts a fling when let go moving. Answers whether the finger tapped:
    // it never scrolled, and it did not land on a fling, which it only stopped.
    bool release(const MouseEvent& event);

    // Ends a fling, and the animation running it.
    void stop();

    // Where a fling carries `offset` in `seconds`, stopped at 0 and `maximum`
    // rather than pushing against either.
    float advance(double seconds, float offset, float maximum);

    // Whether a finger moving this way scrolls content that has `maximum`
    // points to scroll through.
    static bool isScrollDrag(const MouseEvent& event, float maximum);

    bool isFlinging() const { return scroller.isFlinging(); }

private:
    Component& owner;
    TouchScroller scroller;
    bool pressStoppedFling = false;
};
} // namespace eacp::UI
