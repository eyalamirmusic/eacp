#pragma once

#include "GPUBackend-Linux.h"

// The composite Device (plan.md D11): OpenGL for everything that draws and
// Vulkan for everything that dispatches, on one thread, behind one Device.
//
// It is for the machine that has a good OpenGL and no Vulkan worth rendering
// on - a guest whose virtio-gpu speaks virgl, a driver stack that predates
// Vulkan - where the OpenGL has no compute stage either, so an interface on it
// would lose its analytic coverage and every kernel in the tree would have
// nowhere to run. The auto rule picks it exactly there;
// EACP_GPU_BACKEND=composite forces it anywhere, which is how CI exercises
// every crossing path over two llvmpipes.
//
// What it costs is in its name. A resource both sides touch exists twice, and
// what one side wrote is copied through host memory before the other reads it -
// Device::crossingBytesThisFrame() and crossingsThisFrame() are what say how
// much, per frame, so the price is never hidden.
namespace eacp::GPU
{
std::unique_ptr<DeviceBackend> makeCompositeDeviceBackend();
} // namespace eacp::GPU
