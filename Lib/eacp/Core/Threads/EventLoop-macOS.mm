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

// terminate: (Cmd+Q, Dock quit, quit Apple Events) calls exit() without
// unwinding run<T>(); cancel it and stop the loop so the normal teardown runs.
NSApplicationTerminateReply applicationShouldTerminate(id, SEL, NSApplication*)
{
    getEventLoop().quit();
    return NSTerminateCancel;
}

// Dock-icon click while no window is visible. The app decides what "come
// back" means (Apps::setReopenHandler); returning NO suppresses AppKit's
// default un-miniaturize pass.
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

// Installing the termination delegate and activation policy is what makes
// this eacp copy "the app" — only the copy that runs the root loop may do
// it. An eacp inside a dlopen-hosted plugin shares NSApp with the host and
// must never overwrite the host's delegate or policy, so this lives in
// enterRootRunLoop, not in getApp().
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
    configureAppForLoopOwnership();

    // Loop ownership is advertised through the process environment so it
    // crosses eacp copies: a plugin-hosted app's quit reads it to know the
    // running loop is eacp's to stop (see stopProcessRootLoop).
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
    // An eacp copy that never entered a loop — eacp statically linked into
    // a dlopen-hosted plugin — has nothing to quit, and NSApp belongs to
    // the host. Quitting the application is the host's decision.
    if (!s_inRootRunLoop && s_nestedDepth == 0)
        return;

    s_quitRequested = true;

    // [NSApp stop:] applies to the active [NSApp run]. If a nested
    // runFor is on top, its polling loop reads s_quitRequested
    // directly — we only need to wake it via the dummy event.
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

    // NSApp is shared by every copy in the process, so stopping it from
    // here reaches the root loop no matter which eacp copy entered it.
    [getApp() stop:nil];
    [getApp() postEvent:makeWakeEvent() atStart:YES];
}

void scheduleStartup(const Callback& func)
{
    callAsync(func);
}
} // namespace eacp::Threads
