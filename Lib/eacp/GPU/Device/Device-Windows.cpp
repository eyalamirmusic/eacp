#include <eacp/Core/Utils/WinInclude.h>

#include "Device.h"

#include <d3d11.h>

#include <winrt/base.h>

// Windows/D3D11 backend. The GPU device reuses the process-wide D3D11 device the
// graphics layer already created for Windows.UI.Composition (getD3DDevice), so
// buffers, pipelines and the GPUView swapchain all live on the same device and
// can be composited together. nativeQueue() is the immediate context (D3D11 has
// no separate command queue).

namespace eacp::Graphics
{
// Defined in Graphics/D2DFactory-Windows.cpp (linked via eacp-graphics).
ID3D11Device* getD3DDevice();
} // namespace eacp::Graphics

namespace eacp::GPU
{
struct Device::Native
{
    Native()
    {
        // The shared device construction initialises the WinRT compositor and a
        // D3D11 device; on a headless host without a compositor it can throw.
        // Swallow it and leave the device invalid so callers self-skip.
        try
        {
            if (auto* shared = Graphics::getD3DDevice())
            {
                device.copy_from(shared);
                device->GetImmediateContext(context.put());
            }
        }
        catch (...)
        {
        }
    }

    winrt::com_ptr<ID3D11Device> device;
    winrt::com_ptr<ID3D11DeviceContext> context;
};

Device::Device()
    : impl()
{
}

Device& Device::shared()
{
    static Device instance;
    return instance;
}

bool Device::isValid() const
{
    return impl->device != nullptr;
}

void* Device::nativeDevice() const
{
    return impl->device.get();
}

void* Device::nativeQueue() const
{
    return impl->context.get();
}
} // namespace eacp::GPU
