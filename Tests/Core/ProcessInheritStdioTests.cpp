#include "Common.h"
#include "StdioCapture.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <sstream>

using namespace nano;
namespace Proc = eacp::Processes;

namespace
{
std::string tempPath(const std::string& name)
{
    return (std::filesystem::temp_directory_path() / name).string();
}

std::string contentsOf(const std::string& file)
{
    auto stream = std::ifstream {file, std::ios::binary};
    auto buffer = std::ostringstream {};
    buffer << stream.rdbuf();
    return buffer.str();
}

bool contains(const std::string& text, const std::string& needle)
{
    return text.find(needle) != std::string::npos;
}
} // namespace

// With captureOutput off, the child's stdout must land in whatever the
// parent's stdout points at — here, a file the test redirected it to. The
// pre-fix Windows implementation piped it into capture buffers nothing read,
// leaving the file empty.
auto tChildWritesThrough = test("Process/inheritStdio/childWritesThrough") = []
{
    const auto file = tempPath("eacp-inherit-through.txt");
    auto result = Proc::ProcessResult {};
    {
        auto redirect = StdioCapture::StdoutToFile {file};
        auto options = StdioCapture::echoCommand("inherit-proof");
        options.captureOutput = false;
        result = Proc::run(std::move(options));
    }

    check(result.launched);
    check(result.exited);
    check(result.exitCode == 0);
    check(result.output.empty());
    check(contains(contentsOf(file), "inherit-proof"));
    std::filesystem::remove(file);
};

auto tCaptureStaysCaptured = test("Process/inheritStdio/captureStaysCaptured") = []
{
    const auto file = tempPath("eacp-inherit-captured.txt");
    auto result = Proc::ProcessResult {};
    {
        auto redirect = StdioCapture::StdoutToFile {file};
        result = Proc::run(StdioCapture::echoCommand("captured-text"));
    }

    check(result.exitCode == 0);
    check(contains(result.output, "captured-text"));
    check(contentsOf(file).empty());
    std::filesystem::remove(file);
};

// Streaming, not buffering: the marker must be visible while the child is
// still alive — it lingers ~5s after echoing, and the test kills it as soon
// as the marker shows up.
auto tStreamsWhileRunning = test("Process/inheritStdio/streamsWhileRunning") = []
{
    const auto file = tempPath("eacp-inherit-live.txt");
    auto sawItLive = false;
    {
        auto redirect = StdioCapture::StdoutToFile {file};
        auto options = StdioCapture::echoThenLinger("live-proof");
        options.captureOutput = false;
        auto process = Proc::Process {std::move(options)};

        for (auto i = 0; i < 300 && process.isRunning(); ++i)
        {
            if (contains(contentsOf(file), "live-proof"))
            {
                sawItLive = process.isRunning();
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds {10});
        }
        process.kill();
    }

    check(sawItLive);
    std::filesystem::remove(file);
};
