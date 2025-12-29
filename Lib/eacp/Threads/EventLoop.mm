#include "EventLoop.h"
#import <Foundation/Foundation.h>

namespace eacp::Threads
{

void EventLoop::run()
{ CFRunLoopRun(); }

void EventLoop::quit()
{
    auto mainLoop = CFRunLoopGetMain();

    CFRunLoopStop(mainLoop);
    CFRunLoopWakeUp(mainLoop);
}

void EventLoop::call(Callback func)
{
    auto mainLoop = CFRunLoopGetMain();

    CFRunLoopPerformBlock(mainLoop, kCFRunLoopCommonModes, ^{ func(); });
    CFRunLoopWakeUp(mainLoop);
}
} // namespace eacp::Threads