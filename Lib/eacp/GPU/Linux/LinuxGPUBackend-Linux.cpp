#include "LinuxGPUBackend-Linux.h"

#include "../OpenGL/GLBackend-Linux.h"
#include "../Vulkan/VulkanBackend-Linux.h"
#include "CompositeBackend-Linux.h"

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

std::string_view backendNameOf(LinuxGPUBackend backend)
{
    switch (backend)
    {
        case LinuxGPUBackend::OpenGL:
            return "OpenGL";
        case LinuxGPUBackend::Composite:
            return "OpenGL+Vulkan";
        case LinuxGPUBackend::Auto:
        case LinuxGPUBackend::Vulkan:
            break;
    }

    return "Vulkan";
}

std::string_view deviceClassName(GPUDeviceClass found)
{
    switch (found)
    {
        case GPUDeviceClass::Hardware:
            return "hardware";
        case GPUDeviceClass::Software:
            return "software";
        case GPUDeviceClass::None:
            break;
    }

    return "none";
}

// Asked once, and only where nothing was asked for: both probes cost an
// instance or a display each, and an explicit EACP_GPU_BACKEND pays for
// neither. Vulkan is asked first so that the common machine - a real GPU with a
// real Vulkan driver - never opens an EGL display at all.
LinuxGPUBackend autoBackend()
{
    static const auto chosen = []
    {
        const auto vulkan = vulkanDeviceClass();
        const auto gl = vulkan == GPUDeviceClass::Hardware ? GPUDeviceClass::None
                                                           : glDeviceClass();

        const auto backend = chooseAutoBackend(vulkan, gl, glBackendHasCompute());

        LOG("GPU: auto chose ",
            backendNameOf(backend),
            " (Vulkan: ",
            deviceClassName(vulkan),
            ", OpenGL: ",
            vulkan == GPUDeviceClass::Hardware ? "not asked" : deviceClassName(gl),
            ")");

        return backend;
    }();

    return chosen;
}

} // namespace

LinuxGPUBackend getRequestedGPUBackend()
{
    static const auto requested = parseBackendName(getEnvValue("EACP_GPU_BACKEND"));

    return requested;
}

LinuxGPUBackend
    chooseAutoBackend(GPUDeviceClass vulkan, GPUDeviceClass gl, bool glHasCompute)
{
    if (vulkan == GPUDeviceClass::Hardware)
        return LinuxGPUBackend::Vulkan;

    if (gl == GPUDeviceClass::Hardware)
    {
        // A hardware GL with kernels of its own is a whole device; one without
        // is half of one, and the other half is whatever Vulkan there is - even
        // a CPU one, which runs a kernel far faster than not running it at all
        // (D11). With no Vulkan to pair it with there is nothing to compose.
        if (glHasCompute || vulkan == GPUDeviceClass::None)
            return LinuxGPUBackend::OpenGL;

        return LinuxGPUBackend::Composite;
    }

    if (vulkan == GPUDeviceClass::None && gl != GPUDeviceClass::None)
        return LinuxGPUBackend::OpenGL;

    return LinuxGPUBackend::Vulkan;
}

std::unique_ptr<DeviceBackend> makeDeviceBackend()
{
    const auto requested = getRequestedGPUBackend();
    const auto wasAsked = requested != LinuxGPUBackend::Auto;

    switch (wasAsked ? requested : autoBackend())
    {
        case LinuxGPUBackend::OpenGL:
        {
            auto backend = makeGLDeviceBackend();

            if (wasAsked || (backend != nullptr && backend->isValid()))
                return backend;

            // A display EGL named a device on is not yet a context. Where the
            // rule picked GL and no context came up, the software Vulkan the
            // rule weighed it against is still there, and taking it is better
            // than reporting a device that is not - but a copy that *asked*
            // for GL keeps the answer it asked for, invalid and all, so a run
            // pinned to a backend cannot quietly become a run on the other.
            LOG("GPU: the OpenGL backend produced no context; using Vulkan");
            break;
        }

        case LinuxGPUBackend::Composite:
        {
            auto backend = makeCompositeDeviceBackend();

            if (wasAsked || (backend != nullptr && backend->isValid()))
                return backend;

            // Same rule as the OpenGL one below it: the rule's own choice falls
            // back to the Vulkan it was weighed against, and a copy that named
            // the backend keeps it.
            LOG("GPU: the composite device produced no OpenGL context; using "
                "Vulkan");
            break;
        }

        case LinuxGPUBackend::Auto:
        case LinuxGPUBackend::Vulkan:
            break;
    }

    return makeVulkanDeviceBackend();
}
} // namespace eacp::GPU
