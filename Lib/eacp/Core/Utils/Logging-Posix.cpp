#include "LoggingPlatform.h"

namespace eacp::Detail
{

std::tm localTime(std::time_t time)
{
    auto result = std::tm {};
    localtime_r(&time, &result);
    return result;
}

void platformDebugOutput(std::string_view)
{
    // No separate debugger channel on POSIX platforms — the stdout/file
    // writes in Logging.cpp already carry the line.
}

} // namespace eacp::Detail
