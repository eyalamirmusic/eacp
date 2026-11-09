#include "ProcessChildModes.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
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
