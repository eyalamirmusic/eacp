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
//     EACP_GPU_BACKEND=composite the two of them at once: OpenGL for render,
//                               Vulkan for compute (D11)
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

// The auto rule (plan.md D8) as a function of the two answers alone, so what it
// decides can be read - and pinned by a test - on a machine that has neither
// kind of device:
//
//   - Vulkan wherever it has a device that is not a CPU one, because it is the
//     backend with every feature and the one the suite is written against;
//   - else OpenGL where EGL names a device that is not software, which is the
//     virtual machine this was written for: a software Vulkan beside a
//     hardware GL;
//   - else Vulkan, so two software stacks resolve to the baseline one - unless
//     there is no Vulkan at all, where anything GL answered is better than
//     nothing.
//
// The third fact is what the composite added (D11): a hardware GL that has no
// kernels of its own is only half a device, so where a Vulkan - even a CPU one -
// is there to run them, the two are taken together rather than the interface
// losing its analytic coverage. With nothing to pair it with, the GL is still
// the better half and is taken alone.
//
// EACP_VK_SOFTWARE is not weighed here: it says which physical device Vulkan
// takes once Vulkan has been chosen, not whether it is chosen.
LinuxGPUBackend
    chooseAutoBackend(GPUDeviceClass vulkan, GPUDeviceClass gl, bool glHasCompute);

// The Device's backend. One per Device, and it makes every object under it.
std::unique_ptr<DeviceBackend> makeDeviceBackend();
} // namespace eacp::GPU
