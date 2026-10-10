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

// Which way a finger scrolls: up and down, across, or freely in both.
enum class ScrollAxes
{
    vertical,
    horizontal,
    both
};

// A scroll driven by a finger: the content follows the finger once it has moved
// past the slop, and carries on at the speed it was let go with, slowing
// exponentially until it stops. Vertical unless told otherwise; an axis it does
// not scroll along always answers zero.
//
// It owns no position. Every call answers how far the scroll offset should move
// -- positive toward the end of the content, the opposite way to the finger --
// and the component that owns the offset clamps it. Hitting an edge is the
// owner's to notice and answer with stop(), or with stopping the one axis that
// hit it, since only it knows where the edges are.
//
// Time is whatever the caller passes, never read from a clock, so the same
// gesture always produces the same scroll.
class TouchScroller
{
public:
    explicit TouchScroller(ScrollAxes axesToUse = ScrollAxes::vertical);

    // Takes effect from the next press; a gesture keeps the axes it began with.
    void setAxes(ScrollAxes newAxes);
    ScrollAxes getAxes() const;

    // Whether a finger that has moved this far from where it went down is
    // scrolling along `axes`: past the slop, and on a single axis more along it
    // than across. In both, any direction past the slop is a scroll.
    static bool isScrollMovement(Point movement,
                                 ScrollAxes axes = ScrollAxes::vertical);

    // A finger went down. Stops a fling, which is what a press during one means.
    void press(Point position, double time);

    // The finger moved. Zero until the movement is a scroll; from then on every
    // move counts, across or not, so a scroll that has started is not lost to a
    // finger drifting sideways.
    Point drag(Point position, double time);

    // The finger lifted. Starts a fling when it was still moving fast enough,
    // measured over the last moments of the drag, and answers whether it did.
    bool release(Point position, double time);

    // How far a fling carries the scroll in `seconds`, slowing it as it goes.
    // Zero once it has stopped.
    Point advance(double seconds);

    void stop();
    void stopHorizontally();
    void stopVertically();

    bool isDragging() const;
    bool isFlinging() const;

    // Scroll offset per second, positive toward the end of the content.
    Point getVelocity() const;

private:
    struct Sample
    {
        Point position;
        double time = 0.0;
    };

    Point alongAxes(Point point) const;
    Point slopEdge(Point movement) const;
    void addSample(Point position, double time);
    Point releaseVelocity(double now) const;

    Array<Sample, 8> samples;
    int sampleCount = 0;
    int nextSample = 0;

    ScrollAxes axes = ScrollAxes::vertical;
    Point downPosition;
    Point lastPosition;
    Point velocity;
    bool pressed = false;
    bool dragging = false;
};

// A TouchScroller wired to the component that scrolls: what ScrollPanel,
// ListBox and Viewport share, so each keeps only what a tap means to it. Touch
// events only; a mouse is the owner's business.
//
// Every offset passed in is the owner's current one and every offset answered
// is where it should move, which the owner clamps as it always does. The fling
// runs on the owner's animation (Component::startAnimating), and the owner's
// advanceAnimation forwards to advance(). The float overloads are the vertical
// offset alone, for an owner that scrolls up and down only.
class TouchScrolling
{
public:
    explicit TouchScrolling(Component& ownerToUse,
                            ScrollAxes axes = ScrollAxes::vertical);

    // Takes effect from the next press.
    void setAxes(ScrollAxes newAxes);

    void press(const MouseEvent& event);
    float drag(const MouseEvent& event, float offset);
    Point drag(const MouseEvent& event, Point offset);

    // Starts a fling when let go moving. Answers whether the finger tapped:
    // it never scrolled, and it did not land on a fling, which it only stopped.
    bool release(const MouseEvent& event);

    // Ends a fling, and the animation running it.
    void stop();

    // Where a fling carries `offset` in `seconds`, stopped at 0 and `maximum`
    // rather than pushing against either.
    float advance(double seconds, float offset, float maximum);

    // The same on both axes, each stopped at its own edge while the other
    // carries on.
    Point advance(double seconds, Point offset, Point maximum);

    // Whether a finger moving this way scrolls content that has `maximum`
    // points to scroll through: vertically, or along whichever axes of a Point
    // maximum have any room.
    static bool isScrollDrag(const MouseEvent& event, float maximum);
    static bool isScrollDrag(const MouseEvent& event, Point maximum);

    // The axes with room to scroll in `maximum`, vertical when neither has.
    static ScrollAxes axesWithRoom(Point maximum);

    bool isFlinging() const;

private:
    Component& owner;
    TouchScroller scroller;
    bool pressStoppedFling = false;
};
} // namespace eacp::UI
