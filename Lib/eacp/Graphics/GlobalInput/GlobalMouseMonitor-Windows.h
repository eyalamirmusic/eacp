#pragma once

#include <eacp/Core/Utils/WinInclude.h>

#include "GlobalMouseMonitor.h"

#include <optional>

namespace eacp::Graphics
{
// What a low-level mouse hook is handed: the message, and its MSLLHOOKSTRUCT.
struct LowLevelMouseInput
{
    WPARAM message = 0;
    MSLLHOOKSTRUCT data {};
};

// Turns low-level mouse hook input into MouseEvents in desktop points. The hook
// reports physical pixels; `pixelsPerPoint` is the scale they are divided by.
// Remembers the held buttons, so a move with one down is a drag, and the last
// press for `downPos`. Modifiers are left for the caller.
class LowLevelMouseTranslator
{
public:
    std::optional<MouseEvent> translate(const LowLevelMouseInput& input,
                                        float pixelsPerPoint);

private:
    void trackButtons(const MouseEvent& event);

    Point lastPosition;
    Point lastDownPosition;
    MouseButton lastDownButton = MouseButton::Left;
    int buttonsHeld = 0;
};
} // namespace eacp::Graphics
