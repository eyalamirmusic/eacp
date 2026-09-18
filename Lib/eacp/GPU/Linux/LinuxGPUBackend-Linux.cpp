#include "LinuxGPUBackend-Linux.h"

#include "../Vulkan/VulkanBackend-Linux.h"

#include <eacp/Core/Utils/Environment.h>
#include <eacp/Core/Utils/Logging.h>

namespace eacp::GPU
{
namespace
{
LinuxGPUBackend parseBackendName(const std::string& name)
{
    if (name == "vulkan")
        return LinuxGPUBackend::Vulkan;

    if (name == "gl" || name == "opengl")
        return LinuxGPUBackend::OpenGL;

    if (name == "composite")
        return LinuxGPUBackend::Composite;

    if (!name.empty() && name != "auto")
        LOG("EACP_GPU_BACKEND=", name, " names no backend; taking auto");

    return LinuxGPUBackend::Auto;
}

// Said once: a copy that asked for a backend it was not built with would
// otherwise say so on every Device it makes.
void reportNotBuilt(std::string_view name)
{
    static auto reported = false;

    if (reported)
        return;

    reported = true;
    LOG("GPU: no ", name, " backend is built into this copy; using Vulkan");
}
} // namespace

LinuxGPUBackend getRequestedGPUBackend()
{
    static const auto requested = parseBackendName(getEnvValue("EACP_GPU_BACKEND"));

    return requested;
}

std::unique_ptr<DeviceBackend> makeDeviceBackend()
{
    switch (getRequestedGPUBackend())
    {
        case LinuxGPUBackend::OpenGL:
            reportNotBuilt("OpenGL");
            break;

        case LinuxGPUBackend::Composite:
            reportNotBuilt("composite");
            break;

        // Vulkan is the whole of the choice while it is the only backend
        // built: the auto rule that weighs it against a GL device arrives
        // with that backend.
        case LinuxGPUBackend::Auto:
        case LinuxGPUBackend::Vulkan:
            break;
    }

    return makeVulkanDeviceBackend();
}
} // namespace eacp::GPU
