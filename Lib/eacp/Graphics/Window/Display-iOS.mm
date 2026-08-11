#include "Display.h"

#import <UIKit/UIKit.h>

namespace eacp::Graphics
{
Display primaryDisplay()
{
    UIScreen* screen = UIScreen.mainScreen;

    if (screen == nil)
        return {{0.f, 0.f, 1280.f, 800.f}, {0.f, 0.f, 1280.f, 800.f}, 1.f};

    const auto bounds = Rect {(float) CGRectGetMinX(screen.bounds),
                              (float) CGRectGetMinY(screen.bounds),
                              (float) CGRectGetWidth(screen.bounds),
                              (float) CGRectGetHeight(screen.bounds)};

    // A window is the screen here; safe-area insets belong to the view.
    return {bounds, bounds, (float) screen.scale};
}
} // namespace eacp::Graphics
