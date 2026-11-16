#include "SharedDeviceRemoval-Windows.h"

#include <eacp/Core/Utils/Environment.h>
#include <eacp/Core/Utils/FilePath.h>
#include <eacp/Core/Utils/Files.h>
#include <eacp/Graphics/DComp-Windows.h>

#include <d3d12.h>
#include <winrt/base.h>

// Its own binary because it removes the process-wide D3D12 device, and because
// the log file has to be set before that device is first made.

using namespace nano;
using namespace eacp;
using namespace eacp::GPU;
using namespace SharedDeviceRemoval;

namespace
{
int argCount = 0;
char** argValues = nullptr;
int exitCode = 0;

constexpr auto warpVendorId = std::uint32_t {0x1414};

FilePath getTestLogFile()
{
    return FilePath::tempDirectory() / "eacp-GPUAdapterDeviceLossTests.log";
}

bool hasLogLine(std::string_view text)
{
    return Files::readFile(getTestLogFile()).find(text) != std::string::npos;
}

void runTests()
{
    exitCode = nano::run(argCount, argValues);
}
} // namespace

auto tAdapterIsLoggedAndReplacedAfterDeviceLoss =
    test("AdapterAfterDeviceLoss/theLogAndTheQueryNameWarpAfterTheGpuIsRemoved") = []
{
    if (!Device::shared().isValid())
    {
        check(true);
        return;
    }

    const auto hardware = Device::shared().describeAdapter();

    check(hasLogLine("D3D12: adapter 0: "));
    check(hasLogLine("D3D12: chosen adapter: " + hardware.name));
    check(!hardware.driverVersion.empty());
    check(hardware.apiLevel.starts_with("D3D12 feature level "));

    if (!hardware.isSoftware)
        check(hasLogLine("D3D12 driver check: recording a boxed texture copy done"));

    {
        auto onWarp = ReplacementDeviceOnWarp {};
        removeSharedDevice();
        check(Graphics::handleDeviceLossIfNeeded(DXGI_ERROR_DEVICE_REMOVED));
    }

    const auto replacement = Device::shared().describeAdapter();

    LOG("replacement adapter: ", describeAdapterForLog(replacement));

    check(replacement.isSoftware);
    check(replacement.vendorId == warpVendorId);
    check(
        hasLogLine("D3D12: chosen adapter: " + describeAdapterForLog(replacement)));
};

int main(int argc, char* argv[])
{
    argCount = argc;
    argValues = argv;

    Files::removeFile(getTestLogFile());
    setLogFile(getTestLogFile().str());

    Apps::run(runTests);
    return exitCode;
}
