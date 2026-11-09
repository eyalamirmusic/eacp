#pragma once

#include <eacp/Core/Core.h>

// ProcessTests relaunches itself as the child its Process cases need, so the cases
// depend on no shell or system command. TestMain.cpp runs the mode instead of
// the tests when the binary is started with the flag.
namespace ProcessChildModes
{
constexpr auto flag = "--process-child";

constexpr auto echoInput = "echoInput";
constexpr auto writeError = "writeError";
constexpr auto exitWith = "exitWith";
constexpr auto sleepSeconds = "sleepSeconds";
constexpr auto writeBothStreams = "writeBothStreams";
constexpr auto printEnvironment = "printEnvironment";

eacp::Processes::ProcessOptions
    createChildOptions(const std::string& mode,
                       const eacp::Vector<std::string>& modeArguments = {});

bool isChildModeRequested(int argc, char** argv);
int runChildMode(int argc, char** argv);
} // namespace ProcessChildModes
