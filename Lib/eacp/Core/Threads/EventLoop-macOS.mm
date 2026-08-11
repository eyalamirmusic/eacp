#include "EventLoop.h"
#include "../App/App.h"
#include "../ObjC/ObjC.h"
#include "../ObjC/RuntimeClass.h"
#include "../Utils/Environment.h"
#import <Foundation/Foundation.h>
#import <Cocoa/Cocoa.h>

namespace eacp::Threads
{
namespace
{
bool s_inRootRunLoop = false;
int s_nestedDepth = 0;
bool s_quitRequested = false;

// terminate: calls exit() without unwinding run<T>(), so cancel it and stop the
// loop instead, letting the normal teardown run.
NSApplicationTerminateReply applicationShouldTerminate(id, SEL, NSApplication*)
{
    getEventLoop().quit();
    return NSTerminateCancel;
}

// Returning NO suppresses AppKit's default un-miniaturize pass, leaving the
// decision to Apps::setReopenHandler.
BOOL applicationShouldHandleReopen(id, SEL, NSApplication*, BOOL)
{
    Apps::getReopenHandler()();
    return NO;
}

id createAppTerminationBridge()
{
    static auto cls = []
    {
        auto builder =
            new ObjC::RuntimeClass<NSObject>("EacpAppTerminationBridge");
        builder->addProtocol(@protocol(NSApplicationDelegate));
        builder->addMethod(@selector(applicationShouldTerminate:),
                           applicationShouldTerminate);
        builder->addMethod(
            @selector(applicationShouldHandleReopen:hasVisibleWindows:),
            applicationShouldHandleReopen);
        builder->registerClass();
        return builder->get();
    }();

    return [[cls alloc] init];
}

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
    return [NSApplication sharedApplication];
}

// Only the copy that runs the root loop may do this: a dlopen-hosted plugin
// shares NSApp with its host and must never overwrite its delegate or policy.
void configureAppForLoopOwnership()
{
    static auto once = []
    {
        auto* application = getApp();
        [application setActivationPolicy:activationPolicyFromBundle()];

        static auto delegate = ObjC::Ptr<NSObject>(createAppTerminationBridge());
        [application setDelegate:(id<NSApplicationDelegate>) delegate.get()];

        return 0;
    }();
    (void) once;
}

// [NSApp run] is not safe to call recursively; a bounded inner loop is
// EventLoop::runFor.
void enterRootRunLoop()
{
    assert(! s_inRootRunLoop
           && "EventLoop::run must not be called recursively. "
              "Use EventLoop::runFor for nested loops.");

    s_inRootRunLoop = true;
    s_quitRequested = false;
    configureAppForLoopOwnership();

    // Advertised through the process environment so it crosses eacp copies (see
    // stopProcessRootLoop).
    setEnv("EACP_ROOT_LOOP", "1");
    [getApp() run];
    setEnv("EACP_ROOT_LOOP", "0");

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

bool EventLoop::runFor(Time::MS timeout)
{
    s_nestedDepth++;
    s_quitRequested = false;

    auto deadline = Time::Deadline {timeout};

    while (! s_quitRequested)
    {
        if (deadline.expired())
        {
            s_nestedDepth--;
            return false;
        }

        auto remainingSecs = (double) deadline.remaining().count / 1000.0;
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
    // A copy that never entered a loop has nothing to quit, and NSApp belongs to
    // the host. Quitting the application is the host's decision.
    if (!s_inRootRunLoop && s_nestedDepth == 0)
        return;

    s_quitRequested = true;

    // A nested runFor polls s_quitRequested itself, so it only needs the wake
    // event below.
    if (s_nestedDepth == 0 && s_inRootRunLoop)
        [getApp() stop:nil];

    [getApp() postEvent:makeWakeEvent() atStart:YES];
}

bool isEventLoopRunning()
{
    return s_inRootRunLoop || s_nestedDepth > 0
           || getEnvValue("EACP_ROOT_LOOP") == "1";
}

void stopProcessRootLoop()
{
    if (getEnvValue("EACP_ROOT_LOOP") != "1")
        return;

    // NSApp is shared, so this reaches the root loop whichever copy entered it.
    [getApp() stop:nil];
    [getApp() postEvent:makeWakeEvent() atStart:YES];
}

void scheduleStartup(const Callback& func)
{
    callAsync(func);
}
} // namespace eacp::Threads
