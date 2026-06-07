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

} // namespace eacp::Threads
