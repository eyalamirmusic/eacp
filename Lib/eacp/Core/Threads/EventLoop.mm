#include "EventLoop.h"
#import <Foundation/Foundation.h>
#import <Cocoa/Cocoa.h>

namespace eacp::Threads
{
NSApplication* getApp()
{
    return [NSApplication sharedApplication];
}

void EventLoop::run()
{
    [getApp() run];
}



void EventLoop::quit()
{
    [getApp() stop:nil];

    auto event = [NSEvent otherEventWithType:NSEventTypeApplicationDefined
                                    location:NSMakePoint(0, 0)
                               modifierFlags:0
                                   timestamp:0
                                windowNumber:0
                                     context:nil
                                     subtype:0
                                       data1:0
                                       data2:0];
    [getApp() postEvent:event atStart:YES];
}

void EventLoop::call(Callback func)
{
    auto mainLoop = CFRunLoopGetMain();

    CFRunLoopPerformBlock(mainLoop, kCFRunLoopCommonModes, ^{
      func();
    });
    CFRunLoopWakeUp(mainLoop);
}
} // namespace eacp::Threads