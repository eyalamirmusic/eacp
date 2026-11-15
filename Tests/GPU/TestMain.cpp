#include "Common.h"

#ifdef _WIN32
#include <eacp/Core/Utils/Console.h>
#include <eacp/Core/Utils/Environment.h>
#include <eacp/GPU/Dxc/DxcResource.h>

#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <exception>
#endif

namespace
{
int argCount = 0;
char** argValues = nullptr;
int exitCode = 0;

#ifdef _WIN32
// EACP_D3D12_DXC=1 compiles every shader in the suite through DXC rather than
// FXC, which is how the emitter is checked against it. A suite that ends with
// DXC unloaded ran on FXC instead, and fails rather than say otherwise.
bool runsUnderDxc()
{
    return eacp::getEnvValue("EACP_D3D12_DXC") == "1";
}

void enableDxcWhenAsked()
{
    if (!runsUnderDxc())
        return;

    eacp::GPU::Dxc::enableFromOnlineResource();
    const auto status = eacp::GPU::Dxc::load();
    eacp::LOG("EACP_D3D12_DXC=1: DXC ",
              eacp::GPU::Dxc::stateName(status.state),
              " ",
              status.version,
              status.error);
}

int dxcExitCode()
{
    if (!runsUnderDxc())
        return 0;

    auto status = eacp::GPU::Dxc::status();

    if (status.state != eacp::GPU::Dxc::State::failed)
        status = eacp::GPU::Dxc::load();

    if (status.state == eacp::GPU::Dxc::State::loaded)
        return 0;

    eacp::Console::printError("EACP_D3D12_DXC=1 but DXC is "
                              + std::string {eacp::GPU::Dxc::stateName(status.state)}
                              + ": " + status.error);
    return 1;
}

void reportFatal(const char* what)
{
    std::fprintf(stderr, "GPUTests: %s\n", what);
}

void reportAbort(int)
{
    reportFatal("abort()");
}

void reportTerminate()
{
    reportFatal("std::terminate()");
    std::abort();
}

void reportInvalidParameter(
    const wchar_t*, const wchar_t*, const wchar_t*, unsigned int, std::uintptr_t)
{
    reportFatal("a CRT function was handed an invalid parameter");
    std::abort();
}

// A crash ends the process unflushed and a piped stdout is fully buffered, so
// every case that ran before one would vanish with it. Unbuffered, the case
// that crashed is the one after the last line printed.
void reportCrashesAsTheyHappen()
{
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::signal(SIGABRT, reportAbort);
    std::set_terminate(reportTerminate);
    _set_invalid_parameter_handler(reportInvalidParameter);
}
#else
void reportCrashesAsTheyHappen() {}

void enableDxcWhenAsked() {}

int dxcExitCode()
{
    return 0;
}
#endif

void runTests()
{
    enableDxcWhenAsked();
    exitCode = nano::run(argCount, argValues);

    if (exitCode == 0)
        exitCode = dxcExitCode();
}
} // namespace

int main(int argc, char* argv[])
{
    argCount = argc;
    argValues = argv;
    reportCrashesAsTheyHappen();

    eacp::Apps::run(runTests);
    return exitCode;
}
