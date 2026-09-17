#include "SystemAppearance.h"

#import <Foundation/Foundation.h>
#import <TargetConditionals.h>

#if TARGET_OS_IPHONE
#import <UIKit/UIKit.h>
#else
#import <AppKit/AppKit.h>
#endif

namespace eacp::Graphics
{

#if TARGET_OS_IPHONE

bool isSystemDarkMode()
{
    return UIScreen.mainScreen.traitCollection.userInterfaceStyle
           == UIUserInterfaceStyleDark;
}

#else

// Prefer the running app's effective appearance; before NSApp exists
// (e.g. very early startup) fall back to the global interface-style
// default, which is "Dark" only in dark mode.
bool isSystemDarkMode()
{
    if (NSApp != nil)
    {
        auto* match = [NSApp.effectiveAppearance
            bestMatchFromAppearancesWithNames:@[
                NSAppearanceNameAqua, NSAppearanceNameDarkAqua
            ]];
        return [match isEqualToString:NSAppearanceNameDarkAqua];
    }

    auto* style =
        [NSUserDefaults.standardUserDefaults stringForKey:@"AppleInterfaceStyle"];
    return style != nil
           && [style caseInsensitiveCompare:@"Dark"] == NSOrderedSame;
}

#endif

} // namespace eacp::Graphics
