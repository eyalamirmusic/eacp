#pragma once

#include <eacp/Core/Core.h>

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

// The stdio seam the inherit-stdio tests poke through, implemented once per
// platform (StdioCapture-Posix.cpp, StdioCapture-Windows.cpp): redirect this
// process's stdout or stderr to a file at the level a spawned child inherits
// it, and name shell one-liners for the child to run.
namespace StdioCapture
{
enum class Stream
{
    output,
    error
};

struct StreamToFile
{
    StreamToFile(Stream stream, const std::filesystem::path& file);
    ~StreamToFile();

    StreamToFile(const StreamToFile&) = delete;
    StreamToFile& operator=(const StreamToFile&) = delete;

    Stream redirected;
    int savedFd = -1;
    void* savedHandle = nullptr;
};

struct StdoutToFile : StreamToFile
{
    explicit StdoutToFile(const std::filesystem::path& file)
        : StreamToFile(Stream::output, file)
    {
    }
};

struct StderrToFile : StreamToFile
{
    explicit StderrToFile(const std::filesystem::path& file)
        : StreamToFile(Stream::error, file)
    {
    }
};

inline std::filesystem::path tempPath(const std::filesystem::path& name)
{
    return std::filesystem::temp_directory_path() / name;
}

inline std::string contentsOf(const std::filesystem::path& file)
{
    auto stream = std::ifstream {file, std::ios::binary};
    auto buffer = std::ostringstream {};
    buffer << stream.rdbuf();
    return buffer.str();
}

inline bool contains(const std::string& text, const std::string& needle)
{
    return text.find(needle) != std::string::npos;
}

eacp::Processes::ProcessOptions echoCommand(const std::string& text);

// Echo the text, then stay alive until killed or this process is gone. The
// child is StdioLingerHarness, not a shell, so it starts fast, flushes the
// text itself and has no linger of its own to run out.
eacp::Processes::ProcessOptions echoThenLinger(const std::string& text);
} // namespace StdioCapture
