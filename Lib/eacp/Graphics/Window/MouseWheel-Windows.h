#pragma once

#include "../Primitives/Primitives.h"

#include <eacp/Core/Utils/WinInclude.h>

namespace eacp::Graphics
{
// A wheel message's delta in lines, as the other platforms report a notched
// wheel: a detent is WHEEL_DELTA, and a precision touchpad sends fractions of
// one.
inline Point getWheelDeltaInLines(short wheelDelta, bool isHorizontalWheel)
{
    auto lines = static_cast<float>(wheelDelta) / static_cast<float>(WHEEL_DELTA);

    if (isHorizontalWheel)
        return {lines, 0.f};

    return {0.f, lines};
}
} // namespace eacp::Graphics
