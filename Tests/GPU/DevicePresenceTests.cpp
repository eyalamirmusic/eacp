#include "Common.h"

#include <eacp/Core/Utils/Environment.h>

using namespace nano;
using namespace eacp;
using namespace eacp::GPU;

namespace
{
// Sets a variable for the length of a scope and puts back what was there, so a
// test that takes a device capability away cannot leave it away for whatever
// runs next.
struct ScopedEnv
{
    ScopedEnv(std::string_view nameToUse, std::string_view value)
        : name(nameToUse)
        , previous(getEnv(nameToUse))
    {
        setEnv(name, value);
    }

    ~ScopedEnv()
    {
        if (previous.has_value())
            setEnv(name, *previous);
        else
            unsetEnv(name);
    }

    ScopedEnv(const ScopedEnv&) = delete;
    ScopedEnv& operator=(const ScopedEnv&) = delete;

    std::string name;
    std::optional<std::string> previous;
};
} // namespace

// Every other GPU test self-skips without a device, so a lane with no driver
// reports a green suite that ran nothing; EACP_REQUIRE_GPU=1 fails instead.
auto tDeviceIsPresentWhenRequired = test("GPU/aDeviceIsPresentWhenRequired") = []
{
    auto& device = Device::shared();

    // The backend beside the device: one of them is what a lane was asked
    // for and the other is what it got, and a wrong pairing is invisible in
    // either alone.
    LOG("GPU device: ", device.name(), " (", device.backendName(), ")");

    if (getEnvValue("EACP_REQUIRE_GPU") != "1")
        return;

    check(device.isValid(),
          "EACP_REQUIRE_GPU=1 but no GPU device came up - the suite would "
          "otherwise have skipped every test in it and reported a pass");
};

// Compute is the tier every backend but one has, and the query exists for the
// one that has not - the OpenGL backend, whose kernels are a later stage and
// which answers false whatever its context could do - and for the override that
// makes the routes a device without it takes reachable here.
auto tComputeIsSupported = test("GPU/computeIsSupportedAndCanBeTakenAway") = []
{
    auto& device = Device::shared();

    if (!device.isValid())
        return;

    const auto hasKernels = device.backendName() != "OpenGL";

    check(device.supportsCompute() == hasKernels,
          "every backend but OpenGL has kernels");

    auto withoutCompute = ScopedEnv {"EACP_GPU_NO_COMPUTE", "1"};

    check(!device.supportsCompute(), "the override takes the answer away");
};
