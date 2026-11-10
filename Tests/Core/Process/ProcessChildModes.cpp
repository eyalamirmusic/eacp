#include "ProcessChildModes.h"
#include "StopRequest.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <functional>
#include <string>
#include <thread>

namespace ProcessChildModes
{
namespace
{
constexpr auto modeArgumentIndex = 2;
constexpr auto firstModeParameterIndex = 3;
constexpr auto unknownModeExitCode = 2;
constexpr auto inputChunkSize = 4096;
constexpr auto stopRequestTimeoutExitCode = 3;
constexpr auto stopRequestDeadline = std::chrono::seconds {30};

void writeAndFlush(std::FILE* stream, const std::string& text)
{
    std::fputs(text.c_str(), stream);
    std::fflush(stream);
}

int copyInputToOutputUntilEndOfFile(const std::string&)
{
    char chunk[inputChunkSize];

    while (auto bytesRead = std::fread(chunk, 1, sizeof(chunk), stdin))
        std::fwrite(chunk, 1, bytesRead, stdout);

    std::fflush(stdout);
    return 0;
}

int writeLineToError(const std::string& text)
{
    writeAndFlush(stderr, text + "\n");
    return 0;
}

int exitWithCode(const std::string& code)
{
    return std::stoi(code);
}

int sleepForSeconds(const std::string& seconds)
{
    std::this_thread::sleep_for(std::chrono::seconds {std::stoi(seconds)});
    return 0;
}

int writeOneTwoThreeAcrossBothStreams(const std::string&)
{
    writeAndFlush(stdout, "one\n");
    writeAndFlush(stderr, "two\n");
    writeAndFlush(stdout, "three\n");
    return 0;
}

int printEnvironmentVariable(const std::string& name)
{
    writeAndFlush(stdout, eacp::getEnvValue(name));
    return 0;
}

void writeReadyLine()
{
    writeAndFlush(stdout, std::string {readyLine} + '\n');
}

bool waitForStopRequest()
{
    const auto deadline = std::chrono::steady_clock::now() + stopRequestDeadline;

    while (!StopRequest::wasRequested()
           && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds {10});

    return StopRequest::wasRequested();
}

void announceReady(const std::string& markerFile)
{
    std::ofstream {markerFile + readyFileSuffix} << readyLine;
    writeReadyLine();
}

int writeMarkerOnStopRequest(const std::string& markerFile)
{
    StopRequest::installHandler();
    announceReady(markerFile);

    if (!waitForStopRequest())
        return stopRequestTimeoutExitCode;

    std::ofstream {markerFile} << stoppedMarkerText;
    return stoppedExitCode;
}

bool waitFor(const std::function<bool()>& condition)
{
    const auto deadline = std::chrono::steady_clock::now() + stopRequestDeadline;

    while (!condition() && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds {10});

    return condition();
}

// The grandchild is started with no options of its own, so it stays in this
// child's process group: only a request sent to the whole group reaches it.
int writeMarkersOnStopRequestWithGrandchild(const std::string& markerFile)
{
    StopRequest::installHandler();

    auto grandchild = eacp::Processes::Process {
        createChildOptions(awaitStopRequest, {markerFile + grandchildSuffix})};

    if (!waitFor(
            [&]
            { return grandchild.output().find(readyLine) != std::string::npos; }))
        return stopRequestTimeoutExitCode;

    announceReady(markerFile);

    if (!waitForStopRequest())
        return stopRequestTimeoutExitCode;

    waitFor([&] { return !grandchild.isRunning(); });
    std::ofstream {markerFile} << stoppedMarkerText;
    return stoppedExitCode;
}

int sleepForSecondsOnceReady(const std::string& seconds)
{
    writeReadyLine();
    return sleepForSeconds(seconds);
}

struct ChildMode
{
    std::string_view name;
    int (*run)(const std::string& parameter);
};

constexpr ChildMode childModes[] = {
    {echoInput, copyInputToOutputUntilEndOfFile},
    {writeError, writeLineToError},
    {exitWith, exitWithCode},
    {sleepSeconds, sleepForSeconds},
    {writeBothStreams, writeOneTwoThreeAcrossBothStreams},
    {printEnvironment, printEnvironmentVariable},
    {awaitStopRequest, writeMarkerOnStopRequest},
    {awaitStopRequestWithGrandchild, writeMarkersOnStopRequestWithGrandchild},
    {sleepWhenReady, sleepForSecondsOnceReady},
};
} // namespace

eacp::Processes::ProcessOptions
    createChildOptions(const std::string& mode,
                       const eacp::Vector<std::string>& modeArguments)
{
    auto arguments = eacp::Vector<std::string> {flag, mode};

    for (auto& argument: modeArguments)
        arguments.push_back(argument);

    return {eacp::Files::executablePath().str(), arguments};
}

bool isChildModeRequested(int argc, char** argv)
{
    return argc > modeArgumentIndex && std::string_view {argv[1]} == flag;
}

int runChildMode(int argc, char** argv)
{
    const auto mode = std::string_view {argv[modeArgumentIndex]};
    const auto parameter = std::string {
        argc > firstModeParameterIndex ? argv[firstModeParameterIndex] : ""};

    for (auto& childMode: childModes)
        if (childMode.name == mode)
            return childMode.run(parameter);

    return unknownModeExitCode;
}
} // namespace ProcessChildModes
