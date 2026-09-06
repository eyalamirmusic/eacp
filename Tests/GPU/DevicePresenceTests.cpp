#include "Common.h"

#include <eacp/Core/Utils/Environment.h>

using namespace nano;
using namespace eacp;
using namespace eacp::GPU;

// The one test in this directory that does not self-skip, and the answer to the
// failure mode every other one has: a GPU test whose device is missing returns
// immediately and ctest scores it as a pass, so a CI lane whose driver was
// never installed, or whose ICD is a filename that changed with the runner's
// architecture, reports a full green suite that ran nothing at all.
//
// EACP_REQUIRE_GPU=1 says a device is expected here. A lane that sets it and
// finds none fails, with the device's name printed either way so the log says
// which one ran the suite - lavapipe, WARP or a real adapter. Nothing sets it
// by default, so a developer's machine with no driver still gets a green run.
auto tDeviceIsPresentWhenRequired = test("GPU/aDeviceIsPresentWhenRequired") = []
{
    auto& device = Device::shared();

    // Printed before the check, so a failing lane says what it looked for as
    // well as that it did not find it.
    LOG("GPU device: ", device.name());

    if (getEnvValue("EACP_REQUIRE_GPU") != "1")
        return;

    check(device.isValid(),
          "EACP_REQUIRE_GPU=1 but no GPU device came up - the suite would "
          "otherwise have skipped every test in it and reported a pass");
};
