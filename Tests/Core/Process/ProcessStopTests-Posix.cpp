// The POSIX half of requestStop() and newProcessGroup: the child leads a process
// group of its own, and the request is SIGTERM, which needs no group.

#include "ProcessStopping.h"

#include <csignal>
#include <unistd.h>

using namespace nano;
namespace Proc = eacp::Processes;

namespace
{
Proc::Process startSleepingChild(bool newProcessGroup)
{
    auto options = ProcessChildModes::createChildOptions(
        ProcessChildModes::sleepSeconds, {"30"});
    options.newProcessGroup = newProcessGroup;
    return Proc::Process {std::move(options)};
}
} // namespace

auto tNewProcessGroupLeads =
    test("Process/newProcessGroup/childLeadsItsOwnGroup") = []
{
    auto process = startSleepingChild(true);
    check(process.launched());

    const auto pid = (pid_t) process.id();
    check(getpgid(pid) == pid);

    process.kill();
    process.wait();
};

auto tDefaultGroupIsShared =
    test("Process/newProcessGroup/offByDefaultSharesTheLaunchersGroup") = []
{
    auto process = startSleepingChild(false);
    check(process.launched());
    check(getpgid((pid_t) process.id()) == getpgrp());

    process.kill();
    process.wait();
};

auto tRequestStopWithoutAGroup =
    test("Process/requestStop/withoutAGroupStillReachesTheChild") = []
{
    auto dir = scratchDirectory("process-request-stop-no-group");
    auto marker = dir / "stopped.txt";

    auto process = Proc::Process {ProcessStopping::stoppableChild(marker)};
    check(ProcessStopping::becomesReady(process));
    check(process.requestStop());

    check(ProcessStopping::exitCodeOnceStopped(process)
          == ProcessChildModes::stoppedExitCode);
    check(ProcessStopping::stoppedGracefully(marker));

    std::filesystem::remove_all(dir);
};

auto tRequestStopWithoutHandler =
    test("Process/requestStop/childWithoutAHandlerEndsBySigterm") = []
{
    auto process = Proc::Process {ProcessStopping::sleeperInItsOwnGroup()};
    check(ProcessStopping::becomesReady(process));
    check(process.requestStop());

    check(ProcessStopping::exitCodeOnceStopped(process) == 128 + SIGTERM);
};
