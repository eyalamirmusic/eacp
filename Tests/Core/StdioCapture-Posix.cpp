#include "StdioCapture.h"

#include <cstdio>
#include <fcntl.h>
#include <unistd.h>

namespace StdioCapture
{
StdoutToFile::StdoutToFile(const std::string& file)
{
    std::fflush(stdout);
    savedFd = dup(STDOUT_FILENO);

    const auto fd = open(file.c_str(), O_CREAT | O_WRONLY | O_TRUNC, 0644);
    dup2(fd, STDOUT_FILENO);
    close(fd);
}

StdoutToFile::~StdoutToFile()
{
    std::fflush(stdout);
    dup2(savedFd, STDOUT_FILENO);
    close(savedFd);
}

eacp::Processes::ProcessOptions echoCommand(const std::string& text)
{
    return {"/bin/echo", {text}};
}

// The linger execs so kill() reaches the sleep itself; otherwise it survives as
// an orphan holding the inherited stdout, and ctest waits out the whole linger.
eacp::Processes::ProcessOptions echoThenLinger(const std::string& text)
{
    return {
        "/bin/sh",
        {"-c", "echo " + text + "; exec sleep " + std::to_string(lingerSeconds)}};
}
} // namespace StdioCapture
