#include "App.h"

namespace eacp::Apps
{
AppBase::AppBase()
{
    // NSApplication is initialized in EventLoop-macOS.mm before the run loop starts
}
} // namespace eacp::Apps
