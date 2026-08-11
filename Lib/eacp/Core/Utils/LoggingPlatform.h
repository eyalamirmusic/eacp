#pragma once

#include <ctime>
#include <string_view>

namespace eacp::Detail
{

// Thread-safe: localtime_s on Windows, localtime_r elsewhere.
std::tm localTime(std::time_t time);

// OutputDebugString on Windows; a no-op elsewhere.
void platformDebugOutput(std::string_view line);

} // namespace eacp::Detail
