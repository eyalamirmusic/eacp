#pragma once

#include <eacp/Core/Core.h>
#include <string>

// The stdio seam the inherit-stdio tests poke through: redirect this process's
// stdout at the level a spawned child inherits it.
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

// Generous on purpose: a loaded CI runner can take seconds to start the child,
// and watchers kill it the moment they see the marker.
inline constexpr auto lingerSeconds = 60;

eacp::Processes::ProcessOptions echoThenLinger(const std::string& text);
} // namespace StdioCapture
