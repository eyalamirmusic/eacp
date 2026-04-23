#import <AppKit/AppKit.h>

#include "Shell.h"
#include <eacp/Core/ObjC/Strings.h>

namespace eacp::Shell
{

void openExternalURL(const std::string& url)
{
    auto* nsUrl = [NSURL URLWithString:Strings::toNSString(url)];

    if (nsUrl != nil)
        [[NSWorkspace sharedWorkspace] openURL:nsUrl];
}

} // namespace eacp::Shell
