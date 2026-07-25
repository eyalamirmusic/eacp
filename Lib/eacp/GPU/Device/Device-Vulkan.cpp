#include "Device.h"

#include "../Vulkan/VulkanContext.h"

// Vulkan backend. The GPU device is the process-wide logical device and graphics
// queue owned by getVulkanContext(). On Apple this runs over MoltenVK while the
// 2D layer keeps its own Metal device; the two meet only at the CAMetalLayer a
// presenting view hands the swapchain, never as shared Metal objects.

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
    return getVulkanContext().isValid();
}

void* Device::nativeDevice() const
{
    return getVulkanContext().getDevice();
}

void* Device::nativeQueue() const
{
    return getVulkanContext().getQueue();
}

void* Device::nativeTextureCache() const
{
    // Never bound: zero-copy import here goes straight from the platform surface
    // to a VkImage (see TextureImport-Vulkan.h), so there is no cache object
    // standing between them the way Metal has CVMetalTextureCache.
    return nullptr;
}

void* Device::nativeSampler(TextureSampling) const
{
    // Never bound separately: sampling is baked into the shader, so each
    // configuration is an immutable sampler in the descriptor set layout. The
    // same arrangement D3D12 reaches with static samplers. See TextureSampling.
    return nullptr;
}
} // namespace eacp::GPU
