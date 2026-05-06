#include "EventLoop.h"
#import <Foundation/Foundation.h>
#import <Cocoa/Cocoa.h>

namespace eacp::Threads
{
static NSApplication* getApp()
{
    static NSApplication* app = [] {
        auto* application = [NSApplication sharedApplication];
        [application setActivationPolicy:NSApplicationActivationPolicyRegular];
        return application;
    }();
    return app;
}

void EventLoop::run()
{
    [getApp() run];
}

bool EventLoop::runFor(std::chrono::milliseconds timeout)
{
    __block auto timedOut = false;
    auto* self = this;
    auto seconds = (CFTimeInterval) timeout.count() / 1000.0;
    auto fireDate = CFAbsoluteTimeGetCurrent() + seconds;

    auto timer = CFRunLoopTimerCreateWithHandler(
        kCFAllocatorDefault, fireDate, 0, 0, 0,
        ^(CFRunLoopTimerRef) {
            timedOut = true;
            self->quit();
        });

    CFRunLoopAddTimer(CFRunLoopGetMain(), timer, kCFRunLoopCommonModes);

    [getApp() run];

    CFRunLoopRemoveTimer(CFRunLoopGetMain(), timer, kCFRunLoopCommonModes);
    CFRelease(timer);

    return !timedOut;
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
} // namespace eacp::Threads
