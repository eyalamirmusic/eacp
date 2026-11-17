#include <eacp/Core/Process/Process.h>
#include <eacp/Core/Utils/Time.h>

#include <NanoTest/NanoTest.h>

#include <chrono>
#include <string>
#include <thread>

// A window still open when the process exits, closed from after main: from an
// exit handler registered before the window was made, or by the destructor of
// a static built before it. C++ runs both after the destructors of every
// function-local static constructed since, so a backend singleton the close
// reaches - the registry a view deregisters from, the shared device a GPU view
// releases its resources on - has to be one that is never destroyed. On Linux
// six were not, and the close crashed in static destruction.
//
// Exit can only be seen from outside, so ExitHarness is the child.

using namespace nano;

namespace ExitTests
{
namespace
{
constexpr auto exitBudget = eacp::Time::MS {30000};

struct HarnessRun
{
    bool exitedInTime = false;
    int exitCode = -1;
    std::string output;
};

HarnessRun runHarness(const std::string& mode)
{
    auto harness = eacp::Processes::Process {EACP_EXIT_HARNESS, {mode}};
    harness.closeInput();

    auto deadline = eacp::Time::Deadline {exitBudget};

    while (harness.isRunning() && !deadline.expired())
        std::this_thread::sleep_for(std::chrono::milliseconds(10));

    auto run = HarnessRun {};
    run.exitedInTime = !harness.isRunning();

    if (!run.exitedInTime)
        harness.kill();

    run.exitCode = harness.wait();
    run.output = harness.output();

    return run;
}

void checkClosesCleanly(const std::string& mode)
{
    auto run = runHarness(mode);

    check(run.exitedInTime);
    check(run.exitCode == 0);
    check(run.output.find("ExitHarness: open") != std::string::npos);
    check(run.output.find("ExitHarness: closed") != std::string::npos);
}
} // namespace

auto tClosedFromAnExitHandler = test("Exit/windowClosedFromAnExitHandler") = []
{ checkClosesCleanly("atexit"); };

auto tClosedByAnEarlierStatic = test("Exit/windowClosedByAnEarlierStatic") = []
{ checkClosesCleanly("static"); };
} // namespace ExitTests
