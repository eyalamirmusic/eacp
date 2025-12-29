#include "App.h"
#include <Cocoa/Cocoa.h>

namespace eacp::Apps
{
AppBase::AppBase()
{
    [NSApplication sharedApplication];
    [NSApp setActivationPolicy:NSApplicationActivationPolicyRegular];
}
}