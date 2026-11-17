#pragma once

#include <functional>

namespace eacp::Graphics
{
// Releases the process-wide devices drawing brings up on first use - on
// Windows the D3D11, Direct2D and DirectComposition devices behind every view
// and the D3D12 device behind GPU::Device::shared() - each to be built again
// by its next use.
//
// For a plug-in's module exit, before the host unloads the module. Released
// any later, by the module's static destructors, a device is released under
// the loader lock, where the driver waits for worker threads that need that
// lock to exit, and the host hangs. Those holders are never destroyed, so a
// module unloaded without this leaks the devices rather than hanging, but
// only this gives them back.
//
// Every window, view and GPU resource built on the devices has to be gone
// first, except what a process-wide holder gives back through a listener
// below. Main thread only. Does nothing when nothing was built, when called
// again, and on every platform but Windows.
void releaseSharedDevices();

// Registers what releaseSharedDevices runs before it releases the compositor,
// in the order registered: eacp-gpu's release of its D3D12 device, and every
// process-wide holder of objects built on it (a ReleasableShared), such as the
// compute kernels GPUWidgets shares between rasterizations. A holder registers
// once its object first exists, which is after the device it was built on
// registered, so the device's context has waited for its queue to go idle by
// the time the holder's objects go.
//
// Listeners are never unregistered, so only register from storage that is
// never destroyed. Main thread only. Does nothing on every platform but
// Windows, where releaseSharedDevices does nothing either.
void addSharedDevicesReleaseListener(std::function<void()> listener);
} // namespace eacp::Graphics
