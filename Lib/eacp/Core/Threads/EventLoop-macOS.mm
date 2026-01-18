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
