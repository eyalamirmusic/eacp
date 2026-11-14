#include "Common.h"

#include <eacp/Core/Process/Process.h>
#include <eacp/Core/Utils/Time.h>

#include <chrono>
#include <string>
#include <thread>

// A plug-in's unload, end to end. SharedDeviceUnloadFixture is a module with
// eacp linked in, as a plug-in built on it is, and SharedDeviceUnloadHost a
// host with no eacp of its own: it loads the module, has it bring up the
// process-wide devices an editor's first frame does - the D3D12 device behind
// GPU::Device::shared(), and the D3D11, Direct2D and DirectComposition devices
// behind the compositor - runs the module's exit and unloads it with
// FreeLibrary, while the process goes on.
//
// What the module still holds then is released by its static destructors,
// which run under the loader lock. A driver that waits there for its worker
// threads to exit waits for ever, since they need that lock to exit, and a
// validator that unloads a plug-in after its editor tests never exits.
//
// The host gets a process of its own because a deadlocked FreeLibrary cannot
// be ended from inside it: even ExitProcess waits for the loader lock. A host
// still running after unloadBudget is killed, and the case fails. The normal
// path takes well under a second.
//
// Self-skips when the host reports that no device came up at all.

using namespace nano;

namespace SharedDeviceUnloadTests
{
namespace
{
constexpr auto unloadBudget = eacp::Time::MS {30000};

// SharedDeviceUnloadHost's exit code when no device came up.
constexpr auto noDeviceExitCode = 77;

struct HostRun
{
    bool exitedInTime = false;
    int exitCode = -1;
    std::string output;
};

HostRun runHost(const std::string& mode, int cycles)
{
    auto host = eacp::Processes::Process {
        EACP_SHARED_DEVICE_UNLOAD_HOST,
        {EACP_SHARED_DEVICE_UNLOAD_FIXTURE, mode, std::to_string(cycles)}};

    auto deadline = eacp::Time::Deadline {unloadBudget};

    while (host.isRunning() && !deadline.expired())
        std::this_thread::sleep_for(std::chrono::milliseconds(10));

    auto run = HostRun {};
    run.exitedInTime = !host.isRunning();

    if (!run.exitedInTime)
        host.kill();

    run.exitCode = host.wait();
    run.output = host.output();

    return run;
}

void checkHostUnloads(const std::string& mode, int cycles)
{
    auto run = runHost(mode, cycles);

    if (run.exitedInTime && run.exitCode == noDeviceExitCode)
        return;

    check(run.exitedInTime,
          "the host was still running after " + std::to_string(unloadBudget.count)
              + " ms, so it was killed: unloading the module deadlocked. Its "
                "output:\n"
              + run.output);

    check(!run.exitedInTime || run.exitCode == 0, run.output);
}
} // namespace

auto tModuleUnloads =
    test("SharedDeviceUnload/aModuleUnloadsAfterBringingUpTheSharedDevices") = []
{ checkHostUnloads("exit", 1); };

auto tModuleReloads =
    test("SharedDeviceUnload/aModuleUnloadedOnceLoadsAndUnloadsAgain") = []
{ checkHostUnloads("exit", 2); };
} // namespace SharedDeviceUnloadTests
