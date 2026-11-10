// Process::requestStop() on a child started in its own process group: the child
// installs the platform's handler (StopRequest), says it is ready, and on the
// request writes a marker and exits with its own code, so a graceful stop is
// told apart from the child being killed.

#include "ProcessStopping.h"

using namespace nano;
namespace Proc = eacp::Processes;

auto tRequestStopIsGraceful =
    test("Process/requestStop/childInItsOwnGroupStopsGracefully") = []
{
    auto dir = scratchDirectory("process-request-stop");
    auto marker = dir / "stopped.txt";

    auto options = ProcessStopping::stoppableChild(marker);
    options.newProcessGroup = true;

    auto process = Proc::Process {std::move(options)};
    check(process.launched());
    check(ProcessStopping::becomesReady(process));
    check(process.requestStop());

    check(ProcessStopping::exitCodeOnceStopped(process)
          == ProcessChildModes::stoppedExitCode);
    check(ProcessStopping::stoppedGracefully(marker));

    std::filesystem::remove_all(dir);
};

auto tRequestStopAfterExit =
    test("Process/requestStop/falseOnceTheChildHasExited") = []
{
    auto options =
        ProcessChildModes::createChildOptions(ProcessChildModes::exitWith, {"0"});
    options.newProcessGroup = true;

    auto process = Proc::Process {std::move(options)};
    check(process.launched());
    check(process.wait() == 0);
    check(!process.requestStop());
};

auto tNewProcessGroupRunsAndCaptures =
    test("Process/newProcessGroup/runsAndCaptures") = []
{
    auto options =
        ProcessChildModes::createChildOptions(ProcessChildModes::echoInput);
    options.newProcessGroup = true;

    auto process = Proc::Process {std::move(options)};
    check(process.launched());
    check(process.writeToInput("ping\n"));
    process.closeInput();

    check(process.wait() == 0);
    check(process.output().starts_with("ping"));
};

// A server-style child: its output is not captured, so wait() goes straight to
// waiting for the exit. A request from a second thread must still go through
// at once rather than wait for an exit only the request would bring.
auto tRequestStopWhileWaiting =
    test("Process/requestStop/answersWhileAnotherThreadWaits") = []
{
    auto dir = scratchDirectory("process-request-stop-while-waiting");
    auto marker = dir / "stopped.txt";

    auto options = ProcessStopping::stoppableChild(marker);
    options.newProcessGroup = true;
    options.captureOutput = false;

    auto process = Proc::Process {std::move(options)};
    check(ProcessStopping::readyFileAppears(marker));

    auto exitCode = std::async(std::launch::async, [&] { return process.wait(); });
    std::this_thread::sleep_for(std::chrono::milliseconds {200});

    auto sent =
        std::async(std::launch::async, [&] { return process.requestStop(); });
    const auto answered =
        ProcessStopping::arrivesWithin(sent, std::chrono::seconds {3});
    check(answered);

    if (!answered
        || !ProcessStopping::arrivesWithin(exitCode, ProcessStopping::exitDeadline))
        process.kill();

    check(sent.get());
    check(exitCode.get() == ProcessChildModes::stoppedExitCode);
    check(ProcessStopping::stoppedGracefully(marker));

    std::filesystem::remove_all(dir);
};

// The request goes to the whole group: a grandchild the child started, which
// stays in the child's group, stops gracefully too.
auto tRequestStopReachesTheGroup =
    test("Process/requestStop/reachesTheWholeGroup") = []
{
    auto dir = scratchDirectory("process-request-stop-group");
    auto marker = dir / "stopped.txt";

    auto options = ProcessChildModes::createChildOptions(
        ProcessChildModes::awaitStopRequestWithGrandchild, {marker.string()});
    options.newProcessGroup = true;

    auto process = Proc::Process {std::move(options)};
    check(ProcessStopping::becomesReady(process));
    check(process.requestStop());

    check(ProcessStopping::exitCodeOnceStopped(process)
          == ProcessChildModes::stoppedExitCode);
    check(ProcessStopping::stoppedGracefully(marker));
    check(ProcessStopping::stoppedGracefully(
        ProcessStopping::withSuffix(marker, ProcessChildModes::grandchildSuffix)));

    std::filesystem::remove_all(dir);
};

// A child already reaped is never waited on again: a second wait(), or one after
// isRunning() saw the exit, answers at once with the same code (on POSIX the pid
// may by then belong to another child).
auto tWaitTwice = test("Process/wait/aSecondWaitReturnsTheSameCode") = []
{
    auto process = Proc::Process {
        ProcessChildModes::createChildOptions(ProcessChildModes::exitWith, {"7"})};
    check(process.wait() == 7);

    auto again = std::async(std::launch::async, [&] { return process.wait(); });
    check(ProcessStopping::arrivesWithin(again, std::chrono::seconds {5}));
    check(again.get() == 7);
};

auto tWaitAfterIsRunning = test("Process/wait/afterIsRunningSawTheExit") = []
{
    auto process = Proc::Process {
        ProcessChildModes::createChildOptions(ProcessChildModes::exitWith, {"7"})};
    check(ProcessStopping::exitsWithin(process, ProcessStopping::exitDeadline));

    auto code = std::async(std::launch::async, [&] { return process.wait(); });
    check(ProcessStopping::arrivesWithin(code, std::chrono::seconds {5}));
    check(code.get() == 7);
};
