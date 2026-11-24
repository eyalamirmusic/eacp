// Fixture for SharedDeviceUnloadTests: a module with eacp linked in, as a
// plug-in built on it is. Bringing up the devices does what an editor's first
// frame does to the process-wide ones, and rasterizing a path what an editor
// drawing a vector shape does to the compute kernels GPUWidgets shares; the
// module exit is what a plug-in format's module exit does before the host
// unloads it.
#include <eacp/Core/Plugins/PluginExport.h>
#include <eacp/Core/Threads/EventLoop.h>
#include <eacp/GPU/GPU.h>
#include <eacp/GPUWidgets/GPUWidgets.h>
#include <eacp/Graphics/DComp-Windows.h>
#include <eacp/Graphics/Graphics/SharedDevices.h>

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

// Rasterizes an ellipse's fill on the shared Device and waits for it, which
// builds every compute kernel a rasterization shares (sharedKernel). Returns
// 1 once the coverage exists, 0 when it does not.
EACP_PLUGIN_EXPORT int eacpSharedKernelsBringUp()
{
    using namespace eacp;

    auto& device = GPU::Device::shared();

    if (!device.isValid())
        return 0;

    auto path = GPUWidgets::Path {};
    path.addEllipse({2.f, 3.f, 40.f, 28.f});

    auto rasterizer = GPUWidgets::PathRasterizer {};
    rasterizer.setScale(1.f);
    rasterizer.setPath(path);

    if (rasterizer.isEmpty())
        return 0;

    auto commands = device.makeCommandBuffer();

    {
        auto pass = commands.beginCompute();
        rasterizer.dispatch(pass);
    }

    commands.commit();

    return rasterizer.getCoverage().isValid() ? 1 : 0;
}

EACP_PLUGIN_EXPORT void eacpSharedDevicesRelease()
{
    eacp::Graphics::releaseSharedDevices();
}

EACP_PLUGIN_EXPORT void eacpSharedDevicesModuleExit()
{
    eacp::Threads::detachCurrentThreadAsMain();
}
