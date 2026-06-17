#import <UIKit/UIKit.h>

#include "GPUView.h"
#include "GPUViewBacking-Apple.h"

// iOS-specific GPUView piece. The CAMetalLayer setup and rendering live in the
// shared GPUView-Apple.mm; only the backing-scale lookup differs from macOS.

namespace eacp::GPU
{
double platformBackingScale(GPUView& view)
{
    auto* nativeView = (__bridge UIView*) view.getHandle();

    // A realised UIView carries its screen's scale; View-iOS.mm seeds it at
    // creation, so contentScaleFactor is the device-pixel ratio.
    auto scale = nativeView.contentScaleFactor;

    return scale > 0.0 ? scale : 1.0;
}
} // namespace eacp::GPU
