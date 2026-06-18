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
} // namespace eacp::GPU
