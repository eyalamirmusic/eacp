#pragma once

#include "../Common.h"

namespace eacp::UI
{
// All positions are in the receiving component's coordinates, converted by the
// tree walk on the way down (Graphics::MouseEvent is in host view space).
struct MouseEvent
{
    Point position;

    // Where the button went down; carried on every drag.
    Point downPosition;

    // Movement since the previous event, in points.
    Point delta;

    eacp::Graphics::MouseButton button = eacp::Graphics::MouseButton::Left;
    eacp::Graphics::ModifierKeys modifiers;
    int clickCount = 1;

    // Wheel events only, natural-scroll already applied: positive y moves the
    // content toward the start of the document. Units are points when
    // preciseWheel (trackpad), lines otherwise.
    Point wheelDelta;
    bool preciseWheel = false;
};
} // namespace eacp::UI
