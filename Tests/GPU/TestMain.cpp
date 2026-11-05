#include "Common.h"

#ifdef _WIN32
#include <eacp/Core/Utils/Console.h>
#include <eacp/Core/Utils/Environment.h>
#include <eacp/GPU/Dxc/DxcResource.h>
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
#else
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

    eacp::Apps::run(runTests);
    return exitCode;
}
