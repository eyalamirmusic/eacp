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

// Echo the text, then stay alive for ~5 seconds.
eacp::Processes::ProcessOptions echoThenLinger(const std::string& text);
} // namespace StdioCapture
