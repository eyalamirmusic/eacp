#import <Cocoa/Cocoa.h>

#include "GPUView.h"
#include "GPUViewBacking-Apple.h"

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

ViewVisibility platformViewVisibility(GPUView& view)
{
    auto* nativeView = (__bridge NSView*) view.getHandle();
    auto* window = nativeView.window;

    if (window == nil || nativeView.isHiddenOrHasHiddenAncestor)
        return ViewVisibility::NotInVisibleWindow;

    if ((window.occlusionState & NSWindowOcclusionStateVisible) == 0)
        return ViewVisibility::WindowNotVisible;

    return ViewVisibility::Visible;
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
                usingBlock:^(NSNotification*) {
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
