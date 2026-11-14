#include "Common.h"
#include "HarnessScript.h"

#include <chrono>
#include <optional>

using namespace nano;

namespace ProcessDescendantsTests
{
namespace Proc = eacp::Processes;
using Clock = std::chrono::steady_clock;

bool waitUntilExited(const Proc::Process& process)
{
    auto deadline = Clock::now() + std::chrono::seconds {5};

    while (process.isRunning())
    {
        if (Clock::now() > deadline)
            return false;

        std::this_thread::sleep_for(std::chrono::milliseconds {10});
    }

    return true;
}

// The child starts a sleeper that shares its stdout and exits at once, so only
// the sleeper keeps the captured output open when the Process is destroyed.
auto tDestroyAfterExit =
    test("ProcessDescendants/destroyingAfterTheChildExitedEndsWhatItStarted") = []
{
    auto options = HarnessScript {}.spawnSleeping(10000).print("started").options();
    options.killDescendants = true;

    auto process = std::optional<Proc::Process> {};
    process.emplace(options);
    check(process->launched());
    check(waitUntilExited(*process));

    auto started = Clock::now();
    process.reset();
    check(Clock::now() - started < std::chrono::seconds {2});
};

auto tKillAfterExit =
    test("ProcessDescendants/killAfterTheChildExitedEndsWhatItStarted") = []
{
    auto options = HarnessScript {}.spawnSleeping(10000).options();
    options.killDescendants = true;

    auto process = Proc::Process {options};
    check(waitUntilExited(process));

    auto started = Clock::now();
    process.kill();
    process.wait();
    check(Clock::now() - started < std::chrono::seconds {2});
};
} // namespace ProcessDescendantsTests
