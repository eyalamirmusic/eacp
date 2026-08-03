#pragma once

#include "../Common.h"

namespace eacp::UI
{
// A mouse event as the component receiving it sees it.
//
// Distinct from eacp::Graphics::MouseEvent, which is in the host view's
// coordinates, because a component is only ever told about its own space: the
// tree walk converts on the way down, so a knob at the bottom of a nested panel
// still compares position against its own bounds rather than against wherever
// the panel happens to sit this frame.
struct MouseEvent
{
    // Where the pointer is, in the receiving component's coordinates.
    Point position;

    // Where the button went down, in the same space. Carried on every drag so a
    // handler can work from the gesture's origin without storing it.
    Point downPosition;

    // Movement since the previous event, in points.
    Point delta;

    eacp::Graphics::MouseButton button = eacp::Graphics::MouseButton::Left;
    eacp::Graphics::ModifierKeys modifiers;
    int clickCount = 1;

    // Wheel events only. Positive y means the content should move toward the
    // start of the document; the platform has already applied the user's
    // natural-scroll preference. `preciseWheel` says whether the figures are
    // points (trackpad) or lines (a notched wheel) -- see
    // eacp::Graphics::MouseEvent::preciseScrolling.
    Point wheelDelta;
    bool preciseWheel = false;
};
} // namespace eacp::UI
