#include "EventLoop.h"
#import <Foundation/Foundation.h>
#import <UIKit/UIKit.h>

namespace eacp::Threads
{
namespace
{
// The app's startup callback, deferred until a UIScene connects so the window is
// created with a live scene after activation (not from an early run-loop source,
// which iOS warns about and, under the debugger on recent iOS, asserts on).
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

// The scene lifecycle eacp adopts (declared in the bundle's
// UIApplicationSceneManifest). The window is created here, once the scene exists.
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
@end

namespace eacp::Threads
{
void scheduleStartup(const Callback& func)
{
    // Defer to the scene delegate's willConnect rather than posting to the loop,
    // so the app's window is created after the scene activates.
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

bool EventLoop::runFor(std::chrono::milliseconds timeout)
{
    auto seconds = (CFTimeInterval) timeout.count() / 1000.0;
    auto result = CFRunLoopRunInMode(kCFRunLoopDefaultMode, seconds, false);
    return result == kCFRunLoopRunStopped;
}

void EventLoop::quit()
{
    // Mirror the CFRunLoop semantics of the macOS backend: stop only the
    // innermost pump, so stopEventLoop() unwinds a nested runFor (Async::waitFor,
    // runEventLoopUntil) without terminating the app. Never call exit() on iOS —
    // the system and the debugger report it as a crash, and iOS apps are not
    // meant to terminate themselves.
    CFRunLoopStop(CFRunLoopGetCurrent());
}
} // namespace eacp::Threads
