#include "EventLoop.h"
#import <Foundation/Foundation.h>

namespace eacp::Threads
{

void EventLoop::run()
{ CFRunLoopRun(); }

void EventLoop::quit()
{
    CFRunLoopRef mainLoop = CFRunLoopGetMain();

    CFRunLoopStop(mainLoop);
    CFRunLoopWakeUp(mainLoop);
}

void EventLoop::call(const Callback& func)
{
    auto impl = CFRunLoopGetMain();

    CFRunLoopPerformBlock(impl, kCFRunLoopCommonModes, ^{
      if (func)
          func();
    });

    CFRunLoopWakeUp(impl);
}
} // namespace eacp::Threads