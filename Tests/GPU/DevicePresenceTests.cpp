#include "Common.h"

#include <eacp/Core/Utils/Environment.h>

using namespace nano;
using namespace eacp;
using namespace eacp::GPU;

// Every other GPU test self-skips without a device, so a lane with no driver
// reports a green suite that ran nothing; EACP_REQUIRE_GPU=1 fails instead.
auto tDeviceIsPresentWhenRequired = test("GPU/aDeviceIsPresentWhenRequired") = []
{
    auto& device = Device::shared();

    LOG("GPU device: ", device.name());

    if (getEnvValue("EACP_REQUIRE_GPU") != "1")
        return;

    check(device.isValid(),
          "EACP_REQUIRE_GPU=1 but no GPU device came up - the suite would "
          "otherwise have skipped every test in it and reported a pass");
};

auto tDescribeAdapterNamesTheDevice =
    test("GPU/describeAdapterNamesTheDeviceAndItsDriver") = []
{
    auto& device = Device::shared();
    const auto adapter = device.describeAdapter();

    LOG("GPU adapter: ", describeForLog(adapter));

    check(adapter.name == device.name());

    if (!device.isValid())
        return;

    check(!adapter.driverVersion.empty());
    check(!adapter.apiLevel.empty());

    if (!Platform::isApple())
        check(adapter.vendorId != 0);
};
