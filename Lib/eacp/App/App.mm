#include "App.h"
#include <Cocoa/Cocoa.h>

namespace eacp::Apps
{
AppBase::AppBase()
{
    auto app = [NSApplication sharedApplication];
    [NSApp setActivationPolicy:NSApplicationActivationPolicyRegular];
}
} // namespace eacp::Apps