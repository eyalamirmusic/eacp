#import <Cocoa/Cocoa.h>
#import <objc/runtime.h>

#include "GPUView.h"
#include "GPUViewBacking-Apple.h"

#include <eacp/Core/App/AppEnvironment.h>

// macOS-specific GPUView piece. The CAMetalLayer setup and rendering live in the
// shared GPUView-Apple.mm; only the backing-scale lookup differs from iOS.

namespace eacp::GPU
{
double platformBackingScale(GPUView& view)
{
    auto* nativeView = (__bridge NSView*) view.getHandle();

    return nativeView.window != nil ? nativeView.window.backingScaleFactor
                                    : NSScreen.mainScreen.backingScaleFactor;
}

namespace
{
char hasBeenOnScreenKey;

bool isOnScreen(NSWindow* window)
{
    return (window.occlusionState & NSWindowOcclusionStateVisible) != 0;
}

void noteIfOnScreen(NSWindow* window)
{
    if (window != nil && isOnScreen(window))
        objc_setAssociatedObject(
            window, &hasBeenOnScreenKey, @YES, OBJC_ASSOCIATION_RETAIN_NONATOMIC);
}

bool hasBeenOnScreen(NSWindow* window)
{
    return objc_getAssociatedObject(window, &hasBeenOnScreenKey) != nil;
}
} // namespace

ViewVisibility platformViewVisibility(GPUView& view)
{
    auto* nativeView = (__bridge NSView*) view.getHandle();
    auto* window = nativeView.window;

    noteIfOnScreen(window);

    return decideViewVisibility(
        {.headless = Apps::getAppEnvironment().headless,
         .hasWindow = window != nil,
         .viewHidden = nativeView.isHiddenOrHasHiddenAncestor == YES,
         .windowOnScreen = window != nil && isOnScreen(window),
         .windowMinimised = window.isMiniaturized == YES,
         .windowHasBeenOnScreen = window != nil && hasBeenOnScreen(window)});
}

bool platformIsInLiveResize(GPUView& view)
{
    auto* nativeView = (__bridge NSView*) view.getHandle();
    return nativeView.inLiveResize;
}

void* platformObserveWindowVisibility(const Callback& changed)
{
    auto handler = changed;
    id token = [NSNotificationCenter.defaultCenter
        addObserverForName:NSWindowDidChangeOcclusionStateNotification
                    object:nil
                     queue:NSOperationQueue.mainQueue
                usingBlock:^(NSNotification* notification) {
                  noteIfOnScreen((NSWindow*) notification.object);
                  handler();
                }];

    [token retain];
    return (__bridge void*) token;
}

void platformStopObservingWindowVisibility(void* token)
{
    id observer = (__bridge id) token;
    [NSNotificationCenter.defaultCenter removeObserver:observer];
    [observer release];
}
} // namespace eacp::GPU
