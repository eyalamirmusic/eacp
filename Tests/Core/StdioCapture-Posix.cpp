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

// The linger execs, so the shell is replaced rather than left waiting on a
// forked child: kill() reaches a pid that is the sleep itself. Without the
// exec the sleep outlives the kill as an orphan, and it holds the inherited
// stdout until it ends on its own — anything reading that pipe, ctest
// included, waits out the whole linger.
eacp::Processes::ProcessOptions echoThenLinger(const std::string& text)
{
    return {
        "/bin/sh",
        {"-c", "echo " + text + "; exec sleep " + std::to_string(lingerSeconds)}};
}
} // namespace StdioCapture
