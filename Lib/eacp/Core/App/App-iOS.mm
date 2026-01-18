#include "App.h"
#import <UIKit/UIKit.h>

namespace eacp::Apps
{
AppBase::AppBase()
{
    // iOS apps use CFRunLoop directly, no UIApplication setup needed for console apps
}
} // namespace eacp::Apps
