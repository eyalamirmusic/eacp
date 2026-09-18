#pragma once

#include "GPUBackend-Linux.h"

namespace eacp::GPU
{
// Which backend this copy makes its Devices on, asked once per process and
// answered the way the window system's EACP_WINDOW_SYSTEM is - the same kind
// of process-wide question, and the same kind of override.
//
//     EACP_GPU_BACKEND=vulkan   the Vulkan backend
//     EACP_GPU_BACKEND=gl       the OpenGL backend
//     EACP_GPU_BACKEND=auto     the default: whichever suits this machine
//
// EACP_VK_SOFTWARE keeps its meaning inside the Vulkan choice.
enum class LinuxGPUBackend
{
    Auto,
    Vulkan,
    OpenGL,
    Composite
};

LinuxGPUBackend getRequestedGPUBackend();

// The Device's backend. One per Device, and it makes every object under it.
std::unique_ptr<DeviceBackend> makeDeviceBackend();
} // namespace eacp::GPU
