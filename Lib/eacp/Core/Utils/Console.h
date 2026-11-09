#pragma once

#include <string_view>

// Plain program output for command-line tools: what LOG is not, since LOG
// stamps every line with the time and is meant for diagnostics.
namespace eacp::Console
{
// Writes `line` and a newline to stdout and flushes, so the output interleaves
// correctly with a child process's or a pipe reader's.
void print(std::string_view line);

// The same to stderr, for errors and anything a pipe should not capture.
void printError(std::string_view line);
} // namespace eacp::Console
