#import <Foundation/Foundation.h>
#import <ServiceManagement/ServiceManagement.h>

#include "LoginItem.h"

namespace eacp::Apps
{

void setLaunchAtLogin(bool enabled)
{
    if (@available(macOS 13.0, *))
    {
        auto* service = [SMAppService mainAppService];
        NSError* error = nil;

        if (enabled)
            [service registerAndReturnError:&error];
        else
            [service unregisterAndReturnError:&error];
    }
}

bool isLaunchAtLogin()
{
    if (@available(macOS 13.0, *))
        return [SMAppService mainAppService].status == SMAppServiceStatusEnabled;

    return false;
}

} // namespace eacp::Apps
