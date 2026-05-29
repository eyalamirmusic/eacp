#pragma once

#include <string>
#include <string_view>

namespace eacp
{

// Writes a single line to stdout (always) and, on Windows, also to
// OutputDebugString so messages show up in Visual Studio's "Debug
// Output" / DebugView when stdout isn't being captured.
//
// Each line is prefixed with a millisecond-resolution wall-clock
// timestamp so interleaved subsystems can be ordered after the fact.
//
// If file logging has been enabled via setLogFile(), the same line
// is appended to that file (flushed per write so a crash doesn't
// lose the tail).
//
// Safe to call from any thread; a process-wide mutex serialises the
// write path so lines don't interleave.
void LOG(std::string_view text);

// Routes future LOG() calls to also append to `path`. The directory
// is created on demand. Pass an empty string to stop file logging.
// Useful when stdout isn't reachable (CI capture, GUI-only sessions)
// or when a persistent post-mortem record is needed across runs.
void setLogFile(std::string_view path);

} // namespace eacp
