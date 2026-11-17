#include <windows.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>

// A plug-in host with no eacp of its own, for SharedDeviceUnloadTests:
//
//   SharedDeviceUnloadHost <module> <mode> <cycles> [paths]
//
// Each cycle loads the module, has it bring up the shared devices, runs its
// module exit and unloads it with FreeLibrary, while this process goes on, as
// a validator or a DAW scanning plug-ins does. In mode "exit" the module exit
// is all the host calls before unloading. In mode "release" the module
// releases the devices first, brings them up again, which has to build them
// anew, and releases them again, as a plug-in format's module exit does.
// With "paths" the module also rasterizes a path each time it has brought the
// devices up, which builds the compute kernels GPUWidgets shares, and builds
// them anew after a release.
//
// Prints a line per step, so the output of a host that never exits says where
// it stopped. Exits with 0 once every cycle unloaded, with noDeviceExitCode
// when no device came up at all, and with 1 when the module could not be
// loaded or used, or a path did not rasterize on a device that came up.
namespace
{
constexpr auto noDeviceExitCode = 77;

using BringUp = int (*)();
using KernelsBringUp = int (*)();
using Release = void (*)();
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

// Whether a path rasterized, when asked for one and a device came up: there is
// nothing to rasterize on without a device, and the run reports that instead.
bool rasterizes(KernelsBringUp kernelsBringUp, bool paths, int up, int cycle)
{
    if (!paths || up == 0)
        return true;

    auto rasterized = kernelsBringUp();
    say("cycle %d: path rasterized %d", cycle, rasterized);
    return rasterized != 0;
}

int runCycle(const char* path, bool releases, bool paths, int cycle)
{
    auto module = LoadLibraryA(path);

    if (module == nullptr)
    {
        say("cycle %d: LoadLibrary failed (%d)", cycle, (int) GetLastError());
        return 1;
    }

    auto bringUp = entry<BringUp>(module, "eacpSharedDevicesBringUp");
    auto kernelsBringUp = entry<KernelsBringUp>(module, "eacpSharedKernelsBringUp");
    auto release = entry<Release>(module, "eacpSharedDevicesRelease");
    auto moduleExit = entry<ModuleExit>(module, "eacpSharedDevicesModuleExit");

    if (bringUp == nullptr || kernelsBringUp == nullptr || release == nullptr
        || moduleExit == nullptr)
    {
        say("cycle %d: the module lacks an entry", cycle);
        return 1;
    }

    auto up = bringUp();
    say("cycle %d: devices up %d", cycle, up);

    if (!rasterizes(kernelsBringUp, paths, up, cycle))
        return 1;

    if (releases)
    {
        release();
        say("cycle %d: released", cycle);

        auto upAgain = bringUp();
        say("cycle %d: devices up again %d", cycle, upAgain);

        if (!rasterizes(kernelsBringUp, paths, upAgain, cycle))
            return 1;

        release();
        say("cycle %d: released again", cycle);

        if (upAgain != up)
            return 1;
    }

    moduleExit();
    say("cycle %d: module exit done, unloading", cycle);

    FreeLibrary(module);
    say("cycle %d: unloaded", cycle);

    return up == 0 ? noDeviceExitCode : 0;
}

bool isMode(const char* mode)
{
    return std::strcmp(mode, "exit") == 0 || std::strcmp(mode, "release") == 0;
}
} // namespace

int main(int argc, char* argv[])
{
    auto paths = argc == 5 && std::strcmp(argv[4], "paths") == 0;

    if ((argc != 4 && !paths) || !isMode(argv[2]))
    {
        std::printf("usage: SharedDeviceUnloadHost <module> exit|release "
                    "<cycles> [paths]\n");
        return 1;
    }

    auto releases = std::strcmp(argv[2], "release") == 0;
    auto cycles = std::atoi(argv[3]);

    for (auto cycle = 1; cycle <= cycles; ++cycle)
        if (auto result = runCycle(argv[1], releases, paths, cycle); result != 0)
            return result;

    return 0;
}
