#include "Common.h"

#include <eacp/GPU/Vulkan/VulkanContext.h>

// A thread group past the device's maxComputeWorkGroupInvocations is refused
// at pipeline creation with the limit named, rather than handed to a driver
// that may build it wrong. Each dimension is within glslang's own limits, so
// the module compiles and the refusal is the backend's.

using namespace nano;
using namespace eacp;
using namespace eacp::GPU;

namespace
{
struct OversizedGroupKernel final : ComputeProgram
{
    explicit OversizedGroupKernel(ThreadGroupShape shape)
        : ComputeProgram(shape)
    {
        compile();
    }

    void define() override { write(output, threadId(), constant(1.f)); }

    Uniform<OutputBuffer> output;

    EACP_SHADER(output)
};
} // namespace

auto tAGroupPastTheInvocationLimitIsRefused =
    test("WorkGroupLimit/aGroupPastTheInvocationLimitIsRefused") = []
{
    if (!Device::shared().isValid())
        return;

    const auto limit = static_cast<int>(
        getVulkanShared().getProperties().limits.maxComputeWorkGroupInvocations);

    constexpr auto width = 32;
    const auto height = limit / width + 1;

    if (height > 64)
        return;

    auto oversized = OversizedGroupKernel {{width, height}};
    oversized.prepare();
    check(!oversized.isValid());

    auto atTheLimit = OversizedGroupKernel {{width, limit / width}};
    atTheLimit.prepare();
    check(atTheLimit.isValid());
};
