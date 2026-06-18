#include "EventLoop.h"
#import <Foundation/Foundation.h>
#import <Cocoa/Cocoa.h>

#include <cassert>
#include <chrono>

namespace eacp::Threads
{
namespace
{
bool s_inRootRunLoop = false;
int s_nestedDepth = 0;
bool s_quitRequested = false;

NSApplicationActivationPolicy activationPolicyFromBundle()
{
    auto* info = [[NSBundle mainBundle] infoDictionary];

    if ([info[@"LSBackgroundOnly"] boolValue])
        return NSApplicationActivationPolicyProhibited;

    if ([info[@"LSUIElement"] boolValue])
        return NSApplicationActivationPolicyAccessory;

    return NSApplicationActivationPolicyRegular;
}

NSApplication* getApp()
{
    static NSApplication* app = [] {
        auto* application = [NSApplication sharedApplication];
        [application setActivationPolicy:activationPolicyFromBundle()];
        return application;
    }();
    return app;
}

// Single wrapper around [NSApp run]. Apple says NSApplication's run
// is not safe to call recursively, so the guard here keeps the
// invariant in one place: callers that need a bounded inner loop
// use EventLoop::runFor (polled) instead.
void enterRootRunLoop()
{
    assert(! s_inRootRunLoop
           && "EventLoop::run must not be called recursively. "
              "Use EventLoop::runFor for nested loops.");

    s_inRootRunLoop = true;
    s_quitRequested = false;
    [getApp() run];
    s_inRootRunLoop = false;
}

NSEvent* makeWakeEvent()
{
    return [NSEvent otherEventWithType:NSEventTypeApplicationDefined
                              location:NSMakePoint(0, 0)
                         modifierFlags:0
                             timestamp:0
                          windowNumber:0
                               context:nil
                               subtype:0
                                 data1:0
                                 data2:0];
}
} // namespace

void EventLoop::run()
{
    enterRootRunLoop();
}

bool EventLoop::runFor(std::chrono::milliseconds timeout)
{
    s_nestedDepth++;
    s_quitRequested = false;

    auto deadline = std::chrono::steady_clock::now() + timeout;

    while (! s_quitRequested)
    {
        auto now = std::chrono::steady_clock::now();
        if (now >= deadline)
        {
            s_nestedDepth--;
            return false;
        }

        auto remainingSecs =
            std::chrono::duration<double>(deadline - now).count();
        auto* date = [NSDate dateWithTimeIntervalSinceNow:remainingSecs];

        auto* event = [getApp() nextEventMatchingMask:NSEventMaskAny
                                            untilDate:date
                                               inMode:NSDefaultRunLoopMode
                                              dequeue:YES];
        if (event)
            [getApp() sendEvent:event];
    }

    s_quitRequested = false;
    s_nestedDepth--;
    return true;
}

void EventLoop::quit()
{
    s_quitRequested = true;

    // [NSApp stop:] applies to the active [NSApp run]. If a nested
    // runFor is on top, its polling loop reads s_quitRequested
    // directly — we only need to wake it via the dummy event.
    if (s_nestedDepth == 0 && s_inRootRunLoop)
        [getApp() stop:nil];

    [getApp() postEvent:makeWakeEvent() atStart:YES];
}

void scheduleStartup(const Callback& func)
{
    callAsync(func);
}
} // namespace eacp::Threads
