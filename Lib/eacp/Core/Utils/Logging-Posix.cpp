#include "LoggingPlatform.h"

namespace eacp::Detail
{

std::tm localTime(std::time_t time)
{
    auto result = std::tm {};
    localtime_r(&time, &result);
    return result;
}

// No separate debugger channel on POSIX.
void platformDebugOutput(std::string_view) {}

} // namespace eacp::Detail
