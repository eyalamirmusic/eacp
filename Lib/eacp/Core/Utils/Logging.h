#pragma once

#include "Strings.h"

#include <string_view>

namespace eacp
{

// Writes a timestamped line to stdout (and, on Windows, to OutputDebugString
// so it shows up in the debugger when stdout isn't captured). If setLogFile()
// is active, the line is also appended there, flushed per write. Safe to call
// from any thread. This is the sink; prefer LOG() below, which stringifies its
// arguments first.
void logMessage(std::string_view text);

// Logs any mix of strings, numbers and bools — e.g. LOG("status ", code, "/",
// total) — converting each argument via Strings::toString. Callers no longer
// need std::to_string (or manual concatenation) at the call site.
template <typename... Args>
void LOG(const Args&... args)
{
    logMessage(Strings::concat(args...));
}

// Routes future LOG() calls to also append to `path` (directory created on
// demand). Pass an empty string to stop file logging. Useful when stdout
// isn't reachable (CI, GUI-only sessions).
void setLogFile(std::string_view path);

} // namespace eacp
