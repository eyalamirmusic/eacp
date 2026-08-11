#include "EventLoop.h"
#import <Foundation/Foundation.h>
#import <UIKit/UIKit.h>

namespace eacp::Threads
{
namespace
{
// Deferred until a UIScene connects, so the window is created with a live scene
// rather than from an early run-loop source, which recent iOS asserts on.
Callback pendingLaunch;
bool launched = false;
bool appInitialized = false;
} // namespace

void runPendingLaunch()
{
    if (launched)
        return;

    launched = true;

    if (pendingLaunch)
        pendingLaunch();
}
} // namespace eacp::Threads

// The scene lifecycle eacp adopts, declared in the bundle's
// UIApplicationSceneManifest.
@interface EACPSceneDelegate : UIResponder <UIWindowSceneDelegate>
@end

@implementation EACPSceneDelegate
- (void)scene:(UIScene*)scene
    willConnectToSession:(UISceneSession*)session
                 options:(UISceneConnectionOptions*)connectionOptions
{
    eacp::Threads::runPendingLaunch();
}
@end

@interface EACPAppDelegate : UIResponder <UIApplicationDelegate>
@end

@implementation EACPAppDelegate
- (BOOL)application:(UIApplication*)application
    didFinishLaunchingWithOptions:(NSDictionary*)launchOptions
{
    return YES;
}

// Fallback for bundles with no UIScene manifest in Info.plist, where the scene
// delegate's willConnect never fires. UIKit delivers only one of the two.
- (void)applicationDidBecomeActive:(UIApplication*)application
{
    eacp::Threads::runPendingLaunch();
}
@end

namespace eacp::Threads
{
void scheduleStartup(const Callback& func)
{
    pendingLaunch = func;
}

void EventLoop::run()
{
    if (!appInitialized)
    {
        appInitialized = true;
        @autoreleasepool
        {
            char* argv[] = {(char*) "eacp"};
            UIApplicationMain(
                1, argv, nil, NSStringFromClass([EACPAppDelegate class]));
        }
    }
    else
    {
        CFRunLoopRun();
    }
}

bool EventLoop::runFor(Time::MS timeout)
{
    auto seconds = (CFTimeInterval) timeout.count / 1000.0;
    auto result = CFRunLoopRunInMode(kCFRunLoopDefaultMode, seconds, false);
    return result == kCFRunLoopRunStopped;
}

void EventLoop::quit()
{
    // Stops only the innermost pump, so a nested runFor unwinds without
    // terminating the app. Never exit() on iOS: it is reported as a crash.
    CFRunLoopStop(CFRunLoopGetCurrent());
}

// UIApplicationMain runs the loop for the rest of the process's life.
bool isEventLoopRunning()
{
    return appInitialized;
}

// No plugin hosts on iOS — an app is always the process executable.
void stopProcessRootLoop() {}
} // namespace eacp::Threads
