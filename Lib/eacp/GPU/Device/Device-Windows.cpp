#include <eacp/Core/Utils/WinInclude.h>

#include "Device.h"

#include "../Windows/D3D12Context.h"

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
        // A GPU reset kills the 2D layer's D3D11 device and this one together,
        // but the 2D layer also replaces its device voluntarily — so rebuild
        // only when ours is actually gone.
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
    // No zero-copy pixel-buffer cache on D3D12; use Texture::update.
    return nullptr;
}

void* Device::nativeSampler(TextureSampling) const
{
    // D3D12 never binds a sampler: each configuration is a static sampler the
    // shader picks by its SamplerState register. See TextureSampling.
    return nullptr;
}

void Device::trackSubmittedWork(void*)
{
    // Every submit already stamps the queue's fence.
}

void Device::waitForSubmittedWork()
{
    auto& context = getD3D12Context();
    context.waitFor(context.lastSubmitted());
}
} // namespace eacp::GPU
