#include "EventLoop.h"
#import <Foundation/Foundation.h>
#import <Cocoa/Cocoa.h>

namespace eacp::Threads
{
NSApplication* getApp()
{ return [NSApplication sharedApplication]; }

void EventLoop::run()
{ [getApp() run]; }

void EventLoop::quit()
{
    auto mainLoop = CFRunLoopGetMain();

    [getApp() stop:nil];
    CFRunLoopWakeUp(mainLoop);
}

void EventLoop::call(Callback func)
{
    auto mainLoop = CFRunLoopGetMain();

    CFRunLoopPerformBlock(mainLoop, kCFRunLoopCommonModes, ^{ func(); });
    CFRunLoopWakeUp(mainLoop);
}
} // namespace eacp::Threads