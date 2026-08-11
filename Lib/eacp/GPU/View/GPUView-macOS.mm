#import <Cocoa/Cocoa.h>

#include "GPUView.h"
#include "GPUViewBacking-Apple.h"

namespace eacp::GPU
{
double platformBackingScale(GPUView& view)
{
    auto* nativeView = (__bridge NSView*) view.getHandle();

    return nativeView.window != nil ? nativeView.window.backingScaleFactor
                                    : NSScreen.mainScreen.backingScaleFactor;
}
} // namespace eacp::GPU
