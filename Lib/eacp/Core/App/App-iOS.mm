#import <Foundation/Foundation.h>
#import <UIKit/UIKit.h>

#include "App.h"

namespace eacp::Apps
{
// iOS has no Dock / activation policy.
void setDockIconVisible(bool) {}

// The iOS analogue is the home-screen icon badge, which is part of the
// notification system and needs the user's permission before it will show
// anything. Nothing to do here until eacp has notifications.
void setAppBadge(const std::string&) {}

// No OS-level power-off announcement to watch for, so a quit request is
// never the system's (see App.h).
bool isSystemPoweringOff()
{
    return false;
}

void Detail::observeSystemPowerOff() {}

// On iOS every binary is signed, so signature presence means nothing; what
// distinguishes a dev build is the development provisioning profile Xcode
// embeds. Store/TestFlight builds carry none.
bool isDistributionSigned()
{
    return [NSBundle.mainBundle pathForResource:@"embedded"
                                         ofType:@"mobileprovision"]
           == nil;
}

void openExternalURL(const std::string& url)
{
    auto* nsString = [NSString stringWithUTF8String:url.c_str()];

    if (nsString == nil)
        return;

    auto* nsUrl = [NSURL URLWithString:nsString];

    if (nsUrl == nil)
        return;

    [[UIApplication sharedApplication] openURL:nsUrl
                                       options:@{}
                             completionHandler:nil];
}

// iOS has no synchronous file picker (UIDocumentPickerViewController is
// async + delegate-based); not supported under this blocking API.
std::optional<std::string> chooseFile(const FilePickerOptions&)
{
    return std::nullopt;
}

// iOS has no synchronous save panel (UIDocumentPickerViewController is
// async + delegate-based); not supported under this blocking API.
std::optional<std::string> chooseSaveFile(const FileSaveOptions&)
{
    return std::nullopt;
}

// iOS has no synchronous folder picker (UIDocumentPickerViewController is
// async + delegate-based); not supported under this blocking API.
std::optional<std::string> chooseDirectory()
{
    return std::nullopt;
}
} // namespace eacp::Apps
