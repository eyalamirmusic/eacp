#include <eacp/Network/Network.h>

#include <cstdio>
#include <cstdlib>
#include <string>

// A standalone holder of an IPC::Lock, so the tests can watch one behave
// across a real process boundary - including a holder that is killed rather
// than asked to leave, which is the only way to prove the kernel releases a
// lock nobody unlocked.
//
//   IpcLockHarness <name> try
//   IpcLockHarness <name> wait <milliseconds>
//   IpcLockHarness <name> hold <milliseconds>
namespace
{
constexpr auto acquired = 0;
constexpr auto contended = 3;
constexpr auto failed = 4;
} // namespace

int main(int argc, char** argv)
{
    if (argc < 3)
        return failed;

    auto name = std::string {argv[1]};
    auto mode = std::string {argv[2]};
    auto milliseconds = argc > 3 ? std::atoi(argv[3]) : 0;

    try
    {
        auto lock = eacp::IPC::Lock {name};

        if (mode == "try")
        {
            auto guard = eacp::IPC::ScopedLock {lock};
            return guard ? acquired : contended;
        }

        if (mode == "wait")
        {
            auto guard = eacp::IPC::ScopedLock {lock, eacp::Time::MS {milliseconds}};
            return guard ? acquired : contended;
        }

        if (mode == "hold")
        {
            auto guard = eacp::IPC::ScopedLock {lock};

            if (!guard)
                return contended;

            // The parent waits to see this before acting, so a test is timing
            // the lock rather than racing this process's startup.
            std::puts("locked");
            std::fflush(stdout);

            eacp::Time::sleep(eacp::Time::MS {milliseconds});
            return acquired;
        }
    }
    catch (const eacp::IPC::Error&)
    {
        return failed;
    }

    return failed;
}
