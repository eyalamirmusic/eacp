#include "EventLoop.h"
#import <Foundation/Foundation.h>
#import <UIKit/UIKit.h>

@interface EACPAppDelegate : UIResponder <UIApplicationDelegate>
@property(strong, nonatomic) UIWindow* window;
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
static bool appInitialized = false;

void EventLoop::run()
{
    if (!appInitialized)
    {
        
        appInitialized = true;
        @autoreleasepool
        {
            char* argv[] = {(char*)"eacp"};
            UIApplicationMain(1, argv, nil, NSStringFromClass([EACPAppDelegate class]));
        }
    }
    else
    {
        CFRunLoopRun();
    }
}

void EventLoop::quit()
{
    exit(0);
}
} // namespace eacp::Threads
