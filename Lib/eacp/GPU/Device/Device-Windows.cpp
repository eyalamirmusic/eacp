#include "Device.h"

// Windows/DirectX stub. The cross-platform interface compiles and links, but
// no GPU work happens yet. A real D3D12 implementation goes here later.

namespace eacp::GPU
{
struct Device::Native
{
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
    return false;
}

void* Device::nativeDevice() const
{
    return nullptr;
}

void* Device::nativeQueue() const
{
    return nullptr;
}
} // namespace eacp::GPU
