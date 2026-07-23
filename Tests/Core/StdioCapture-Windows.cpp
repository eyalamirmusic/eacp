#include "StdioCapture.h"

#include <cstdio>
#include <fcntl.h>
#include <io.h>
#include <sys/stat.h>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

namespace StdioCapture
{
// Both levels must move: the CRT fd so our own printf lands in the file, and
// the Win32 std handle, which is what a spawned child actually inherits.
StdoutToFile::StdoutToFile(const std::string& file)
{
    std::fflush(stdout);
    savedHandle = GetStdHandle(STD_OUTPUT_HANDLE);
    savedFd = _dup(1);

    auto fd = -1;
    _sopen_s(&fd,
             file.c_str(),
             _O_CREAT | _O_WRONLY | _O_TRUNC,
             _SH_DENYNO,
             _S_IREAD | _S_IWRITE);
    _dup2(fd, 1);
    _close(fd);

    SetStdHandle(STD_OUTPUT_HANDLE, (HANDLE) _get_osfhandle(1));
}

StdoutToFile::~StdoutToFile()
{
    std::fflush(stdout);
    _dup2(savedFd, 1);
    _close(savedFd);
    SetStdHandle(STD_OUTPUT_HANDLE, (HANDLE) savedHandle);
}

eacp::Processes::ProcessOptions echoCommand(const std::string& text)
{
    return {"cmd.exe", {"/c", "echo " + text}};
}

eacp::Processes::ProcessOptions echoThenLinger(const std::string& text)
{
    return {"cmd.exe", {"/c", "echo " + text + " & ping -n 6 127.0.0.1 > nul"}};
}
} // namespace StdioCapture
