#pragma once

#include "../View/View.h"

namespace eacp::Graphics
{

// Reports the mouse events of the kinds in `typesToReport` from anywhere on the
// desktop, this app's windows included, while it is alive.
// pos and downPos are desktop points, measured as Display measures them.
// The handler runs on the main thread, after the event, never after destruction.
// Never fires on Linux, iOS or Android, nor for Entered and Exited.
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
