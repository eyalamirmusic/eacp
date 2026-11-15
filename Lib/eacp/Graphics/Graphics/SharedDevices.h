#pragma once

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
// first. Main thread only. Does nothing when nothing was built, when called
// again, and on every platform but Windows.
void releaseSharedDevices();
} // namespace eacp::Graphics
