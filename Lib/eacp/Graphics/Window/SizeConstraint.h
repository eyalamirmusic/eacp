#pragma once

#include "../Primitives/Primitives.h"

#include <functional>

namespace eacp::Graphics
{

// Which side of a window the user is moving. A constraint that has to give
// one dimension up to keep the other reads this to decide which: deriving the
// width from the height while the user drags the right edge makes the window
// appear to resist the cursor.
//
// Both is a corner drag, or a resize with no edge at all (a maximise, a
// programmatic size); a constraint treats it as Width.
enum class ResizeAxis
{
    Width,
    Height,
    Both
};

// The size a window is about to take, handed to WindowOptions::sizeConstraint
// before anything has been resized. Content points, not the outer frame.
struct ResizeRequest
{
    Point size;
    ResizeAxis axis = ResizeAxis::Both;
};

// A pure rule from a proposed content size to the one the window may take.
// It is called before the size is committed, on every path a window can take
// a size on (see WindowOptions::sizeConstraint), and returns rather than
// acting - it has no window to act on - so nothing in it can trigger the
// resize it is constraining. Return request.size to accept it.
using SizeConstraint = std::function<Point(const ResizeRequest&)>;

// The axis of a resize whose edge the platform did not say (AppKit hands over
// a size and nothing else), read off which dimension moved. Both if both did
// or neither.
//
// `dragStart` is the size when the drag began, never the size the window
// holds now: the constraint moves the other dimension, so against the current
// size every proposal looks like a corner drag and the axis flips on each
// event.
ResizeAxis resizeAxisBetween(Point dragStart, Point proposed);

// The largest constrained size no bigger than `available` on either side -
// what a maximise, a zoom, a fullscreen or a display too small for the window
// gives a constrained window. Tries width-driven first and height-driven when
// that overflows; a constraint that fits neither way is clamped.
Point fitWithin(const SizeConstraint& constraint, Point available);

// Locks a window's proportions, minus any fixed border: the content less
// `fixed` keeps `ratio`, the border keeps its thickness, and the window can
// only be dragged into sizes where both hold.
//
// The plain lock - Insets {} - is a game's pixel grid, a video, a fixed-aspect
// canvas: content that fills its window and has a shape of its own. The
// bordered one is that canvas under a toolbar, or beside an inspector, whose
// height (or width) does not scale with it. A view otherwise has to letterbox
// itself against every window the user drags out, and letterboxing is drawing
// the bars *and* mapping input past them; a window that cannot take the wrong
// shape removes the problem rather than handling it.
//
// One instance answers both questions the split raises, so the border is
// written once: it is the SizeConstraint the window enforces (it converts),
// and lockedArea is where the locked content goes in the resized view.
//
// Only the ratio's proportion is read, so {16, 9} and {1920, 1080} mean the
// same thing. A ratio with a non-positive side describes no shape and locks
// nothing.
struct AspectRatioLock
{
    AspectRatioLock(Point ratioToUse, Insets fixedToUse = {});

    Point operator()(const ResizeRequest& request) const;

    // The rect the locked content occupies within `bounds`: the bounds less
    // the border, and - should the bounds not be a size the lock would have
    // allowed - the largest rect of the ratio that fits inside that, centred.
    // So the layout is right even for a size the window took behind the
    // lock's back.
    //
    // Whole points either way. The lock rounds to the point, so a size it
    // allowed is up to half a point off the exact ratio, and re-deriving the
    // ratio from it would put the content on a fractional pixel - a scissor
    // clip cuts the edge of what is painted there.
    Rect lockedArea(const Rect& bounds) const;

    // Whether `size` (content, border included) is one the lock hands back
    // unchanged from either axis - a size the window can actually hold.
    bool allows(Point size) const;

    bool isLocking() const;

    Point ratio;
    Insets fixed;
};

} // namespace eacp::Graphics
