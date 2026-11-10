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
constexpr auto awaitStopRequest = "awaitStopRequest";
constexpr auto sleepWhenReady = "sleepWhenReady";
constexpr auto awaitStopRequestWithGrandchild = "awaitStopRequestWithGrandchild";

// awaitStopRequest prints this line once its stop handler is installed (with
// "\r\n" after it on Windows, whose stdout is in text mode), then writes
// stoppedMarkerText to the file it was given and exits with stoppedExitCode
// when the request arrives (30 s at most). sleepWhenReady prints it and sleeps
// for the seconds it was given, with no handler.
constexpr auto readyLine = "ready";
constexpr auto stoppedMarkerText = "stopped";
constexpr auto stoppedExitCode = 42;

// awaitStopRequest also writes readyLine to the file named by the marker plus
// readyFileSuffix once its handler is in, for a parent that does not capture its
// output. awaitStopRequestWithGrandchild starts an awaitStopRequest grandchild
// whose marker is its own plus grandchildSuffix, says it is ready once the
// grandchild is, and on the request waits for the grandchild to exit before
// writing its own marker.
constexpr auto readyFileSuffix = ".ready";
constexpr auto grandchildSuffix = ".grandchild";

eacp::Processes::ProcessOptions
    createChildOptions(const std::string& mode,
                       const eacp::Vector<std::string>& modeArguments = {});

bool isChildModeRequested(int argc, char** argv);
int runChildMode(int argc, char** argv);
} // namespace ProcessChildModes
