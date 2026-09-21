#include "Common.h"

#include <eacp/GPU/Linux/LinuxGPUBackend-Linux.h>
#include <eacp/GPU/OpenGL/GLBackend-Linux.h>
#include <eacp/GPU/Vulkan/VulkanBackend-Linux.h>

using namespace nano;
using namespace eacp;
using namespace eacp::GPU;

// Which backend a Device gets on Linux (plan.md D8). The rule itself is a
// function of two answers and nothing else, so it is pinned here over every
// pair a machine could give - including the pairs this machine cannot produce,
// which is the whole reason it is a function rather than a branch inside the
// probes.
//
// Beside it, the two probes run for real on whatever this machine is, and the
// Device that came up is checked against what the rule says of them.
namespace
{
constexpr auto hardware = GPUDeviceClass::Hardware;
constexpr auto software = GPUDeviceClass::Software;
constexpr auto none = GPUDeviceClass::None;

// The third fact the rule weighs, and what the OpenGL backend answers today:
// no kernels of its own until stage 6 builds D9's tier.
constexpr auto withCompute = true;
constexpr auto noCompute = false;
} // namespace

auto tVulkanWinsOnHardware = test("Backend/hardwareVulkanTakesEveryPair") = []
{
    // Whatever GL answered: Vulkan is the backend with every feature and the
    // one the suite is written against, so a real Vulkan device ends it.
    check(chooseAutoBackend(hardware, hardware, withCompute)
          == LinuxGPUBackend::Vulkan);
    check(chooseAutoBackend(hardware, software, noCompute)
          == LinuxGPUBackend::Vulkan);
    check(chooseAutoBackend(hardware, none, noCompute) == LinuxGPUBackend::Vulkan);
};

// The machine this backend was written for: a software Vulkan beside a
// hardware GL, which is what a guest whose virtio-gpu speaks virgl and not
// Venus offers. A GL with kernels of its own is the whole device there.
auto tGLWinsWhereVulkanIsSoftware = test("Backend/hardwareGLBeatsACPUVulkan") = []
{
    check(chooseAutoBackend(software, hardware, withCompute)
          == LinuxGPUBackend::OpenGL);

    // And with no Vulkan at all there is nothing to compose it with, kernels
    // or none.
    check(chooseAutoBackend(none, hardware, withCompute) == LinuxGPUBackend::OpenGL);
    check(chooseAutoBackend(none, hardware, noCompute) == LinuxGPUBackend::OpenGL);
};

// The composite (plan.md D11), and the one pair that reaches it: a hardware GL
// that has no compute stage of its own, beside a Vulkan that can run the
// kernels even though it would be a poor thing to render with. That is this
// development VM exactly, and it is the only machine the rule picks it on -
// EACP_GPU_BACKEND=composite is how CI reaches it over two llvmpipes.
auto tComputelessGLPairsWithACPUVulkan =
    test("Backend/aGLWithoutKernelsPairsWithACPUVulkan") = []
{
    check(chooseAutoBackend(software, hardware, noCompute)
          == LinuxGPUBackend::Composite);
};

// What CI is: llvmpipe on one side and llvmpipe on the other. Vulkan is the
// baseline, so two software stacks resolve to it.
auto tSoftwareBothWaysIsVulkan = test("Backend/twoSoftwareStacksChooseVulkan") = []
{
    check(chooseAutoBackend(software, software, noCompute)
          == LinuxGPUBackend::Vulkan);
    check(chooseAutoBackend(software, none, noCompute) == LinuxGPUBackend::Vulkan);

    // A software GL is not worth composing with either: llvmpipe's Vulkan has
    // the kernels and the fuller feature set, so there is nothing to gain by
    // crossing between two CPU stacks.
    check(chooseAutoBackend(software, software, withCompute)
          == LinuxGPUBackend::Vulkan);

    // And a machine with nothing at all: Vulkan, so the failure is reported by
    // the backend every other lane reports it from.
    check(chooseAutoBackend(none, none, noCompute) == LinuxGPUBackend::Vulkan);
};

// The one case where the ranking gives way: no Vulkan to be had at all, and a
// GL that is only software is still a device.
auto tSoftwareGLBeatsNoVulkan = test("Backend/aSoftwareGLBeatsNoVulkanAtAll") = []
{ check(chooseAutoBackend(none, software, noCompute) == LinuxGPUBackend::OpenGL); };

// The probes themselves, on whatever this machine has. Neither is allowed to
// crash or to hang on a machine with no device of its kind, which is the only
// thing a lane with neither can check.
auto tProbesAnswerTheSameTwice = test("Backend/eachProbeAnswersTheSameTwice") = []
{
    check(vulkanDeviceClass() == vulkanDeviceClass(),
          "the Vulkan probe is a question, not a state change");

    check(glDeviceClass() == glDeviceClass(),
          "and so is the EGL one - the display it opened is joined, not "
          "reopened");

    check(!glBackendHasCompute(),
          "D9's tier is stage 6's, so the OpenGL backend has no kernels and "
          "the composite is what a machine with a hardware GL gets");
};

// What actually came up, against what the rule says of this machine. Only
// checked where nothing was asked for: EACP_GPU_BACKEND names the backend
// itself, and every GL lane on CI does.
auto tTheDeviceIsWhatTheRuleChose = test("Backend/theDeviceIsTheOneAutoChose") = []
{
    auto& device = Device::shared();

    if (!device.isValid() || getRequestedGPUBackend() != LinuxGPUBackend::Auto)
        return;

    const auto vulkan = vulkanDeviceClass();
    const auto chosen = chooseAutoBackend(
        vulkan, vulkan == hardware ? none : glDeviceClass(), glBackendHasCompute());

    const auto expected = chosen == LinuxGPUBackend::OpenGL      ? "OpenGL"
                          : chosen == LinuxGPUBackend::Composite ? "OpenGL+Vulkan"
                                                                 : "Vulkan";

    LOG("Backend: auto wanted ", expected, ", got ", device.backendName());

    // Not an equality: a rule that picked a backend with a GL half on a machine
    // whose context will not come up falls back to Vulkan, which is the one
    // deviation there is.
    if (device.backendName() != expected)
        check(chosen != LinuxGPUBackend::Vulkan,
              "only a GL that produced no context is allowed to differ");
};
