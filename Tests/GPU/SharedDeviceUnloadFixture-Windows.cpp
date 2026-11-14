// Fixture for SharedDeviceUnloadTests: a module with eacp linked in, as a
// plug-in built on it is. Bringing up the devices does what an editor's first
// frame does to the process-wide ones; the module exit is what a plug-in
// format's module exit does before the host unloads it.
#include <eacp/Core/Plugins/PluginExport.h>
#include <eacp/Core/Threads/EventLoop.h>
#include <eacp/GPU/GPU.h>
#include <eacp/Graphics/DComp-Windows.h>

namespace
{
// What came up, as SharedDeviceUnloadHost prints it.
constexpr auto d3d12DeviceUp = 1;
constexpr auto d3d11DeviceUp = 2;
constexpr auto compositionDeviceUp = 4;
} // namespace

EACP_PLUGIN_EXPORT int eacpSharedDevicesBringUp()
{
    eacp::Threads::attachCurrentThreadAsMain();

    auto up = 0;

    if (eacp::GPU::Device::shared().isValid())
        up |= d3d12DeviceUp;

    if (eacp::Graphics::getD3DDevice() != nullptr)
        up |= d3d11DeviceUp;

    if (eacp::Graphics::getCompositionDevice() != nullptr)
        up |= compositionDeviceUp;

    return up;
}

EACP_PLUGIN_EXPORT void eacpSharedDevicesModuleExit()
{
    eacp::Threads::detachCurrentThreadAsMain();
}
