#include "EventLoop.h"

namespace eacp::Threads
{

// CFRunLoopStop is reference-counted, so quit() already stops only the innermost
// pump. Windows needs the two split and overrides this.
void stopEventLoop()
{
    getEventLoop().quit();
}

// The main run loop is a process singleton here, so a hosted copy's callAsync
// and timers already reach the host's loop.
void attachCurrentThreadAsMain() {}

} // namespace eacp::Threads
