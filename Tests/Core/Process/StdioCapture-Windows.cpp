#include "StdioCapture.h"

#include <cstdio>
#include <fcntl.h>
#include <io.h>
#include <sys/stat.h>

#include <eacp/Core/Utils/WinInclude.h>

namespace StdioCapture
{
namespace
{
DWORD getStdHandleIdOf(Stream stream)
{
    if (stream == Stream::error)
        return STD_ERROR_HANDLE;

    return STD_OUTPUT_HANDLE;
}

std::FILE* getFileOf(Stream stream)
{
    if (stream == Stream::error)
        return stderr;

    return stdout;
}

int getDescriptorOf(Stream stream)
{
    return _fileno(getFileOf(stream));
}
} // namespace

// Both levels must move: the CRT fd so our own printf lands in the file, and
// the Win32 std handle, which is what a spawned child actually inherits.
StreamToFile::StreamToFile(Stream stream, const std::filesystem::path& file)
    : redirected(stream)
{
    const auto descriptor = getDescriptorOf(redirected);

    std::fflush(getFileOf(redirected));
    savedHandle = GetStdHandle(getStdHandleIdOf(redirected));
    savedFd = _dup(descriptor);

    auto fd = -1;
    _wsopen_s(&fd,
              file.c_str(),
              _O_CREAT | _O_WRONLY | _O_TRUNC,
              _SH_DENYNO,
              _S_IREAD | _S_IWRITE);
    _dup2(fd, descriptor);
    _close(fd);

    SetStdHandle(getStdHandleIdOf(redirected), (HANDLE) _get_osfhandle(descriptor));
}

StreamToFile::~StreamToFile()
{
    std::fflush(getFileOf(redirected));
    _dup2(savedFd, getDescriptorOf(redirected));
    _close(savedFd);
    SetStdHandle(getStdHandleIdOf(redirected), (HANDLE) savedHandle);
}

eacp::Processes::ProcessOptions echoCommand(const std::string& text)
{
    return {"cmd.exe", {"/c", "echo " + text}};
}

eacp::Processes::ProcessOptions echoThenLinger(const std::string& text)
{
    return {EACP_STDIO_LINGER_HARNESS,
            {text, std::to_string(GetCurrentProcessId())}};
}
} // namespace StdioCapture
