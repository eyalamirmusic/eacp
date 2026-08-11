#import <Foundation/Foundation.h>
#import <UIKit/UIKit.h>

#include "App.h"

namespace eacp::Apps
{
// iOS has no Dock / activation policy.
void setDockIconVisible(bool) {}

// Every iOS binary is signed, so presence means nothing: a dev build is the one
// carrying the development provisioning profile Xcode embeds.
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

// UIDocumentPickerViewController is async, so iOS supports none of the three
// blocking pickers below.
std::optional<std::string> chooseFile(const FilePickerOptions&)
{
    return std::nullopt;
}

std::optional<std::string> chooseSaveFile(const FileSaveOptions&)
{
    return std::nullopt;
}

std::optional<std::string> chooseDirectory()
{
    return std::nullopt;
}
} // namespace eacp::Apps
