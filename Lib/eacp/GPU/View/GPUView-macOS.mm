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

bool platformWindowIsOpaque(GPUView& view)
{
    auto* nativeView = (__bridge NSView*) view.getHandle();

    // WindowOptions::transparentBackground and cornerRadius are both this, set
    // in Window-macOS.mm's constructor, so there is nothing else to ask.
    return nativeView.window == nil || nativeView.window.isOpaque;
}
} // namespace eacp::GPU
