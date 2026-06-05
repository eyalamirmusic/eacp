#import <Foundation/Foundation.h>
#import <UIKit/UIKit.h>

#include "App.h"

namespace eacp::Apps
{
// iOS has no Dock / activation policy.
void setDockIconVisible(bool) {}

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

// iOS has no synchronous folder picker (UIDocumentPickerViewController is
// async + delegate-based); not supported under this blocking API.
std::optional<std::string> chooseDirectory()
{
    return std::nullopt;
}
} // namespace eacp::Apps
