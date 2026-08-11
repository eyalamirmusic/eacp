#pragma once

#include "Strings.h"

#include <string_view>

namespace eacp
{

// Writes a timestamped line to stdout, to OutputDebugString on Windows, and to
// any setLogFile() target, flushed per write. Safe to call from any thread.
// This is the sink; prefer LOG() below, which stringifies its arguments first.
void logMessage(std::string_view text);

// Logs any mix of strings, numbers and bools: LOG("status ", code, "/", total).
template <typename... Args>
void LOG(const Args&... args)
{
    logMessage(Strings::concat(args...));
}

// Creates the directory on demand. Pass an empty string to stop file logging.
void setLogFile(std::string_view path);

} // namespace eacp
