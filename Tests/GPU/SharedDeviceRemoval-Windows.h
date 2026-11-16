#pragma once

#include "Common.h"

#include <d3d12.h>
#include <winrt/base.h>

// For the device-loss binaries: each removes the process-wide D3D12 device and
// recovers onto WARP, as the machine that crashed after its GPU hung did.
namespace SharedDeviceRemoval
{
inline void removeSharedDevice()
{
    auto* device =
        static_cast<ID3D12Device*>(eacp::GPU::Device::shared().nativeDevice());
    auto removableDevice = winrt::com_ptr<ID3D12Device5>();

    if (device != nullptr
        && SUCCEEDED(device->QueryInterface(__uuidof(ID3D12Device5),
                                            removableDevice.put_void())))
    {
        removableDevice->RemoveDevice();
    }
}

struct ReplacementDeviceOnWarp
{
    ReplacementDeviceOnWarp() { eacp::setEnv("EACP_D3D12_WARP", "1"); }
    ~ReplacementDeviceOnWarp() { eacp::unsetEnv("EACP_D3D12_WARP"); }
};
} // namespace SharedDeviceRemoval
