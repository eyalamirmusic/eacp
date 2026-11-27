#include "StdioCapture.h"

#include <cstdio>
#include <fcntl.h>
#include <unistd.h>

namespace StdioCapture
{
namespace
{
std::FILE* getFileOf(Stream stream)
{
    if (stream == Stream::error)
        return stderr;

    return stdout;
}

int getDescriptorOf(Stream stream)
{
    return fileno(getFileOf(stream));
}
} // namespace

StreamToFile::StreamToFile(Stream stream, const std::filesystem::path& file)
    : redirected(stream)
{
    std::fflush(getFileOf(redirected));
    savedFd = dup(getDescriptorOf(redirected));

    const auto fd = open(file.c_str(), O_CREAT | O_WRONLY | O_TRUNC, 0644);
    dup2(fd, getDescriptorOf(redirected));
    close(fd);
}

StreamToFile::~StreamToFile()
{
    std::fflush(getFileOf(redirected));
    dup2(savedFd, getDescriptorOf(redirected));
    close(savedFd);
}

eacp::Processes::ProcessOptions echoCommand(const std::string& text)
{
    return {"/bin/echo", {text}};
}

eacp::Processes::ProcessOptions echoThenLinger(const std::string& text)
{
    return {EACP_STDIO_LINGER_HARNESS, {text, std::to_string(getpid())}};
}
} // namespace StdioCapture
