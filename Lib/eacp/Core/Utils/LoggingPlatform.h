#pragma once

#include <ctime>
#include <string_view>

namespace eacp::Detail
{

// Platform-provided pieces of the logger, implemented in Logging-Windows.cpp
// and Logging-Posix.cpp so Logging.cpp itself carries no platform switches.

// Thread-safe conversion of a time_t to local broken-down time
// (localtime_s on Windows, localtime_r elsewhere).
std::tm localTime(std::time_t time);

// Mirrors a finished log line to the platform debugger channel
// (OutputDebugString on Windows; a no-op elsewhere).
void platformDebugOutput(std::string_view line);

} // namespace eacp::Detail
