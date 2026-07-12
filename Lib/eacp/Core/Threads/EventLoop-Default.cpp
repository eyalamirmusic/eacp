#include "EventLoop.h"

namespace eacp::Threads
{

// Platforms with CFRunLoop semantics: quit() only affects the innermost pump
// (CFRunLoopStop is reference-counted), so stopEventLoop and EventLoop::quit
// share an implementation. Windows needs a split — stopEventLoop must only
// unblock the innermost runFor without terminating the outer run() — and
// provides its own override in EventLoop-Windows.cpp.
void stopEventLoop()
{
    getEventLoop().quit();
}

// The main run loop is a process singleton here (CFRunLoopGetMain /
// the glib main context), so a hosted eacp copy's callAsync and timers
// already reach the host's loop without any setup.
void attachCurrentThreadAsMain() {}

} // namespace eacp::Threads
