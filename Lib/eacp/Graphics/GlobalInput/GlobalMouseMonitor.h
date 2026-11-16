#pragma once

#include "../View/View.h"

namespace eacp::Graphics
{

// Reports mouse events anywhere on the desktop, in any app, for as long as it
// is alive: for a window that must get out of the way the moment the user does
// anything else, or for following the pointer outside the app's own windows.
//
// Only the kinds in `typesToReport` arrive, so a monitor that asks for presses
// alone is not woken for every move. Entered and Exited never arrive: they
// belong to a view, not to the desktop.
//
// `pos` and `downPos` are desktop points: from the primary display's top-left,
// growing right and down, as Display and WindowOptions::initialPosition measure
// them. The handler runs on the main thread, after the event, and never after
// the monitor is destroyed.
//
// macOS: NSEvent global and local monitors, so presses on every app's windows
// and on this app's own are seen; mouse monitors need no Accessibility
// permission. Windows: a low-level mouse hook (WH_MOUSE_LL), which reports no
// clicks per press, pressure or gesture, and whose `rawDelta` is `delta`.
// Linux, iOS and Android: never fires.
class GlobalMouseMonitor
{
public:
    using Handler = std::function<void(const MouseEvent&)>;

    GlobalMouseMonitor(EA::Vector<MouseEventType> typesToReport, Handler onEvent);
    ~GlobalMouseMonitor();

    GlobalMouseMonitor(const GlobalMouseMonitor&) = delete;
    GlobalMouseMonitor& operator=(const GlobalMouseMonitor&) = delete;

private:
    struct Native;
    Pimpl<Native> impl;
};

} // namespace eacp::Graphics
