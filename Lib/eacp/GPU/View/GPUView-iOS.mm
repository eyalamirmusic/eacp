#import <UIKit/UIKit.h>

#include "GPUView.h"
#include "GPUViewBacking-Apple.h"

namespace eacp::GPU
{
double platformBackingScale(GPUView& view)
{
    auto* nativeView = (__bridge UIView*) view.getHandle();

    // View-iOS.mm seeds this at creation from the screen's scale.
    auto scale = nativeView.contentScaleFactor;

    return scale > 0.0 ? scale : 1.0;
}
} // namespace eacp::GPU
