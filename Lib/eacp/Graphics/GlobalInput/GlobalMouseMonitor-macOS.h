#pragma once

#include "GlobalMouseMonitor.h"

#import <AppKit/AppKit.h>

#include <optional>

namespace eacp::Graphics
{
// The NSEvent types to monitor for `types`. The presses come too whenever
// releases or drags are asked for, because their `downPos` is the last press.
NSEventMask getEventMaskForTypes(const EA::Vector<MouseEventType>& types);

// Turns what a global or local NSEvent monitor sees into MouseEvents in desktop
// points, remembering the last press for `downPos`.
class GlobalMouseEventTranslator
{
public:
    std::optional<MouseEvent> translate(NSEvent* event);

private:
    Point lastDownPosition;
};
} // namespace eacp::Graphics
