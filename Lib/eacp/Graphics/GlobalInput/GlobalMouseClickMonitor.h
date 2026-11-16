#pragma once

#include "../Common.h"

namespace eacp::Graphics
{

// Calls back on every mouse button press anywhere on the desktop, in any app,
// for as long as it is alive. For a window that must get out of the way the
// moment the user does anything else. Presses only, no position or button.
//
// macOS: an NSEvent global monitor, which needs no Accessibility permission
// but does not see presses on this app's own windows; handle those in the
// view. Windows: a low-level mouse hook, which sees every press. Linux, iOS
// and Android: never fires.
class GlobalMouseClickMonitor
{
public:
    explicit GlobalMouseClickMonitor(Callback onPressed);
    ~GlobalMouseClickMonitor();

    GlobalMouseClickMonitor(const GlobalMouseClickMonitor&) = delete;
    GlobalMouseClickMonitor& operator=(const GlobalMouseClickMonitor&) = delete;

private:
    struct Native;
    Pimpl<Native> impl;
};

} // namespace eacp::Graphics
