#include <windows.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>

// A plug-in host with no eacp of its own, for SharedDeviceUnloadTests:
//
//   SharedDeviceUnloadHost <module> <mode> <cycles>
//
// Each cycle loads the module, has it bring up the shared devices, runs its
// module exit and unloads it with FreeLibrary, while this process goes on, as
// a validator or a DAW scanning plug-ins does. In mode "exit" the module exit
// is all the host calls before unloading.
//
// Prints a line per step, so the output of a host that never exits says where
// it stopped. Exits with 0 once every cycle unloaded, with noDeviceExitCode
// when no device came up at all, and with 1 when the module could not be
// loaded or lacks an entry.
namespace
{
constexpr auto noDeviceExitCode = 77;

using BringUp = int (*)();
using ModuleExit = void (*)();

void say(const char* format, int cycle, int value = 0)
{
    std::printf(format, cycle, value);
    std::printf("\n");
    std::fflush(stdout);
}

template <typename Function>
Function entry(HMODULE module, const char* name)
{
    return reinterpret_cast<Function>(GetProcAddress(module, name));
}

int runCycle(const char* path, int cycle)
{
    auto module = LoadLibraryA(path);

    if (module == nullptr)
    {
        say("cycle %d: LoadLibrary failed (%d)", cycle, (int) GetLastError());
        return 1;
    }

    auto bringUp = entry<BringUp>(module, "eacpSharedDevicesBringUp");
    auto moduleExit = entry<ModuleExit>(module, "eacpSharedDevicesModuleExit");

    if (bringUp == nullptr || moduleExit == nullptr)
    {
        say("cycle %d: the module lacks an entry", cycle);
        return 1;
    }

    auto up = bringUp();
    say("cycle %d: devices up %d", cycle, up);

    moduleExit();
    say("cycle %d: module exit done, unloading", cycle);

    FreeLibrary(module);
    say("cycle %d: unloaded", cycle);

    return up == 0 ? noDeviceExitCode : 0;
}
} // namespace

int main(int argc, char* argv[])
{
    if (argc != 4 || std::strcmp(argv[2], "exit") != 0)
    {
        std::printf("usage: SharedDeviceUnloadHost <module> exit <cycles>\n");
        return 1;
    }

    auto cycles = std::atoi(argv[3]);

    for (auto cycle = 1; cycle <= cycles; ++cycle)
        if (auto result = runCycle(argv[1], cycle); result != 0)
            return result;

    return 0;
}
