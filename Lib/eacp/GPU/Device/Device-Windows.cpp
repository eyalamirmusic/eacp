#include <eacp/Core/Utils/WinInclude.h>

#include "Device.h"

#include "../Windows/D3D12Context.h"

#include <functional>

// Windows/D3D12 backend. The GPU device is the process-wide D3D12 device and
// direct queue owned by getD3D12Context(). The 2D graphics layer keeps its own
// D3D11 device for Direct2D; the compositor composes output from both, so the
// devices never need to be shared. nativeQueue() is the direct command queue.

namespace eacp::Graphics
{
// Defined in Graphics/D2DFactory-Windows.cpp (linked via eacp-graphics).
void addRenderingDeviceReplacedListener(std::function<void()> listener);
} // namespace eacp::Graphics

namespace eacp::GPU
{
// Defined in View/GPUView-Windows.cpp: rebuilds every live GPUView's swapchain
// against the recreated device.
void refreshAllGPUViewsForNewDevice();

struct Device::Native
{
    Native()
    {
        // A GPU reset kills the 2D layer's D3D11 device and this D3D12 device
        // together. The graphics layer's recovery fires this listener after it
        // re-established its own device; if ours died too (or never existed),
        // rebuild it and every GPUView swapchain. The 2D layer also replaces
        // its device voluntarily, so a healthy D3D12 device is left alone.
        Graphics::addRenderingDeviceReplacedListener(
            []
            {
                auto& context = getD3D12Context();

                if (context.isValid()
                    && SUCCEEDED(context.getDevice()->GetDeviceRemovedReason()))
                    return;

                context.recreateAfterDeviceLoss();
                refreshAllGPUViewsForNewDevice();
            });
    }
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
    return getD3D12Context().isValid();
}

void* Device::nativeDevice() const
{
    return getD3D12Context().getDevice();
}

void* Device::nativeQueue() const
{
    return getD3D12Context().getQueue();
}

void* Device::nativeTextureCache() const
{
    // No zero-copy pixel-buffer cache on the D3D12 backend yet; the camera/video
    // path uploads frames through Texture::update instead.
    return nullptr;
}
} // namespace eacp::GPU
