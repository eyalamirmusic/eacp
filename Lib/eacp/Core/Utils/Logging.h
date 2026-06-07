#pragma once

#include <string>
#include <string_view>

namespace eacp
{

// Writes a timestamped line to stdout (and, on Windows, to OutputDebugString
// so it shows up in the debugger when stdout isn't captured). If setLogFile()
// is active, the line is also appended there, flushed per write. Safe to call
// from any thread.
void LOG(std::string_view text);

// Routes future LOG() calls to also append to `path` (directory created on
// demand). Pass an empty string to stop file logging. Useful when stdout
// isn't reachable (CI, GUI-only sessions).
void setLogFile(std::string_view path);

} // namespace eacp
