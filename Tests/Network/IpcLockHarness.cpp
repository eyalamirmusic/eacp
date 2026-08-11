#include <eacp/Network/Network.h>

#include <cstdio>
#include <cstdlib>
#include <string>

// A standalone holder of an IPC::Lock, killable so the tests can prove the
// kernel releases a lock nobody unlocked:
//   IpcLockHarness <name> try | wait <ms> | hold <ms>
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

            // The parent waits for this before acting, so a test times the lock
            // rather than this process's startup.
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
