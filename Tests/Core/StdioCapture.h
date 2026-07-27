#pragma once

#include <eacp/Core/Core.h>
#include <string>

// The stdio seam the inherit-stdio tests poke through, implemented once per
// platform (StdioCapture-Posix.cpp, StdioCapture-Windows.cpp): redirect this
// process's stdout to a file at the level a spawned child inherits it, and
// name shell one-liners for the child to run.
namespace StdioCapture
{
struct StdoutToFile
{
    explicit StdoutToFile(const std::string& file);
    ~StdoutToFile();

    StdoutToFile(const StdoutToFile&) = delete;
    StdoutToFile& operator=(const StdoutToFile&) = delete;

    int savedFd = -1;
    void* savedHandle = nullptr;
};

eacp::Processes::ProcessOptions echoCommand(const std::string& text);

// How long the echoThenLinger child stays alive after echoing. Generous on
// purpose: a loaded CI runner can take seconds just to start the child, and
// the marker window has to outlast that. Watchers kill the child the moment
// they see the marker, so the happy path never pays for it.
inline constexpr auto lingerSeconds = 60;

// Echo the text, then stay alive for lingerSeconds.
eacp::Processes::ProcessOptions echoThenLinger(const std::string& text);
} // namespace StdioCapture
