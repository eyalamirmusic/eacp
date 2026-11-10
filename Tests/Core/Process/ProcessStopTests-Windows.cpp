// The Windows half of requestStop(): CTRL_BREAK_EVENT reaches only a child in a
// process group of its own that shares this process's console, so every other
// child is refused rather than signalled (an event for a group that is not one
// would go to every process on the console, this one included).

#include "ProcessStopping.h"

#include <eacp/Core/Utils/WinInclude.h>

using namespace nano;
namespace Proc = eacp::Processes;

namespace
{
constexpr auto unansweredWait = std::chrono::milliseconds {500};

bool stillRunsUnsignalled(Proc::Process& process,
                          const std::filesystem::path& marker)
{
    const auto exited = ProcessStopping::exitsWithin(process, unansweredWait);
    return !exited && !std::filesystem::exists(marker);
}
} // namespace

auto tRequestStopNeedsAGroup = test("Process/requestStop/refusedWithoutAGroup") = []
{
    auto dir = scratchDirectory("process-request-stop-no-group");
    auto marker = dir / "stopped.txt";

    auto process = Proc::Process {ProcessStopping::stoppableChild(marker)};
    check(ProcessStopping::becomesReady(process));

    check(!process.requestStop());
    check(stillRunsUnsignalled(process, marker));

    process.kill();
    process.wait();
    std::filesystem::remove_all(dir);
};

auto tRequestStopNeedsTheConsole =
    test("Process/requestStop/refusedForANoWindowChild") = []
{
    auto dir = scratchDirectory("process-request-stop-no-window");
    auto marker = dir / "stopped.txt";

    auto options = ProcessStopping::stoppableChild(marker);
    options.newProcessGroup = true;
    options.noWindow = true;

    auto process = Proc::Process {std::move(options)};
    check(ProcessStopping::becomesReady(process));

    check(!process.requestStop());
    check(stillRunsUnsignalled(process, marker));

    process.kill();
    process.wait();
    std::filesystem::remove_all(dir);
};

auto tRequestStopDetached = test("Process/requestStop/refusedForADetachedChild") = []
{
    auto options = ProcessChildModes::createChildOptions(
        ProcessChildModes::sleepSeconds, {"1"});
    options.newProcessGroup = true;
    options.detached = true;

    auto process = Proc::Process {std::move(options)};
    check(process.launched());
    check(!process.requestStop());
};

// With no handler of its own the child gets the default one, which ends it the
// way Ctrl+Break in its console would.
auto tRequestStopWithoutHandler =
    test("Process/requestStop/childWithoutAHandlerEndsAsCtrlBreakDoes") = []
{
    auto process = Proc::Process {ProcessStopping::sleeperInItsOwnGroup()};
    check(ProcessStopping::becomesReady(process));
    check(process.requestStop());

    check((DWORD) ProcessStopping::exitCodeOnceStopped(process)
          == (DWORD) STATUS_CONTROL_C_EXIT);
};
