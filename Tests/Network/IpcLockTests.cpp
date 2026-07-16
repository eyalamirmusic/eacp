#include "Common.h"

using namespace nano;
using eacp::IPC::Lock;
using eacp::IPC::ScopedLock;
namespace Proc = eacp::Processes;

namespace
{
// Mirrors IpcLockHarness's exit codes.
constexpr auto acquired = 0;
constexpr auto contended = 3;

Proc::ProcessResult
    runHarness(const std::string& name, const std::string& mode, int ms = 0)
{
    auto arguments = eacp::Vector<std::string> {name, mode};

    if (ms > 0)
        arguments.add(std::to_string(ms));

    return Proc::run(EACP_IPC_LOCK_HARNESS, arguments);
}

Proc::Process holdInAnotherProcess(const std::string& name, int ms)
{
    return Proc::Process {EACP_IPC_LOCK_HARNESS, {name, "hold", std::to_string(ms)}};
}

// The harness prints "locked" once it genuinely holds the lock; until then a
// test that acted would be racing process startup rather than the lock.
bool waitUntilHeld(Proc::Process& harness)
{
    auto deadline = eacp::Time::Deadline {eacp::Time::MS {5000}};

    while (!deadline.expired())
    {
        if (harness.output().find("locked") != std::string::npos)
            return true;

        eacp::Time::sleep(eacp::Time::MS {10});
    }

    return false;
}
} // namespace

auto tGuardTakesAndReleases = test("Ipc/Lock/guardTakesAndReleases") = []
{
    auto lock = Lock {"eacp.tests.ipc.takesAndReleases"};

    {
        auto guard = ScopedLock {lock};
        check(guard.isLocked());
    }

    auto again = ScopedLock {lock};
    check(again.isLocked());
};

// The platforms disagree underneath: flock() re-locking a handle it already
// owns succeeds, LockFileEx() reports a violation. Both must say no.
auto tNoRecursion = test("Ipc/Lock/secondGuardOnOneLockIsRefused") = []
{
    auto lock = Lock {"eacp.tests.ipc.noRecursion"};

    auto first = ScopedLock {lock};
    check(first.isLocked());

    {
        auto second = ScopedLock {lock};
        check(!second.isLocked());
    }

    // The refused guard must not have released the winner's lock on its way
    // out - the whole point of tracking who actually holds it.
    check(runHarness("eacp.tests.ipc.noRecursion", "try").exitCode == contended);
};

auto tSeparateObjectsContend = test("Ipc/Lock/separateLockObjectsContend") = []
{
    auto first = Lock {"eacp.tests.ipc.twoObjects"};
    auto second = Lock {"eacp.tests.ipc.twoObjects"};

    auto firstGuard = ScopedLock {first};
    check(firstGuard.isLocked());

    auto secondGuard = ScopedLock {second};
    check(!secondGuard.isLocked());
};

auto tDifferentNames = test("Ipc/Lock/differentNamesDoNotContend") = []
{
    auto first = Lock {"eacp.tests.ipc.nameA"};
    auto second = Lock {"eacp.tests.ipc.nameB"};

    auto firstGuard = ScopedLock {first};
    auto secondGuard = ScopedLock {second};

    check(firstGuard.isLocked());
    check(secondGuard.isLocked());
};

auto tExcludesAnotherProcess = test("Ipc/Lock/excludesAnotherProcess") = []
{
    auto lock = Lock {"eacp.tests.ipc.crossProcess"};
    auto guard = ScopedLock {lock};
    check(guard.isLocked());

    auto result = runHarness("eacp.tests.ipc.crossProcess", "try");
    check(result.launched);
    check(result.exitCode == contended);
};

auto tReleasedIsAvailable =
    test("Ipc/Lock/releasedLockIsAvailableToAnotherProcess") = []
{
    auto lock = Lock {"eacp.tests.ipc.released"};

    {
        auto guard = ScopedLock {lock};
        check(guard.isLocked());
    }

    check(runHarness("eacp.tests.ipc.released", "try").exitCode == acquired);
};

// The headline claim: a holder that dies without unlocking strands nothing.
auto tKilledHolderReleases = test("Ipc/Lock/killedHolderReleases") = []
{
    auto name = std::string {"eacp.tests.ipc.killed"};
    auto harness = holdInAnotherProcess(name, 60000);

    check(harness.launched());
    check(waitUntilHeld(harness));

    auto lock = Lock {name};

    {
        auto guard = ScopedLock {lock};
        check(!guard.isLocked());
    }

    harness.kill();
    harness.wait();

    auto guard = ScopedLock {lock, eacp::Time::MS {5000}};
    check(guard.isLocked());
};

auto tTimeoutWaitsForRelease = test("Ipc/Lock/timeoutWaitsForHolderToRelease") = []
{
    auto name = std::string {"eacp.tests.ipc.waitForRelease"};
    auto harness = holdInAnotherProcess(name, 300);

    check(waitUntilHeld(harness));

    auto lock = Lock {name};
    auto guard = ScopedLock {lock, eacp::Time::MS {10000}};

    check(guard.isLocked());
    check(harness.wait() == acquired);
};

auto tTimeoutGivesUp = test("Ipc/Lock/timeoutGivesUp") = []
{
    auto lock = Lock {"eacp.tests.ipc.givesUp"};
    auto guard = ScopedLock {lock};
    check(guard.isLocked());

    check(runHarness("eacp.tests.ipc.givesUp", "wait", 200).exitCode == contended);
};

auto tEmptyNameThrows = test("Ipc/Lock/emptyNameThrows") = []
{
    auto threw = false;

    try
    {
        auto lock = Lock {""};
    }
    catch (const eacp::IPC::Error&)
    {
        threw = true;
    }

    check(threw);
};

// A name is folded into a filename, so a separator must not survive to steer
// the lock file out of the lock directory. Both names fold to one file, which
// is what proves the folding happened.
auto tFoldsSeparators = test("Ipc/Lock/foldsPathSeparatorsInNames") = []
{
    auto escaping = Lock {"../eacp.tests.ipc.escape"};
    auto folded = Lock {".._eacp.tests.ipc.escape"};

    auto firstGuard = ScopedLock {escaping};
    check(firstGuard.isLocked());

    auto secondGuard = ScopedLock {folded};
    check(!secondGuard.isLocked());
};
