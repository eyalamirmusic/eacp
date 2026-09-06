#include "Device.h"

#include "../Vulkan/VulkanContext.h"

// Linux/Vulkan backend. A Device owns its VulkanContext - its command pools,
// its timeline semaphore, its upload arena, its constant ring and its staging
// and readback pools - and shares the VkInstance, the VkPhysicalDevice, the
// VkDevice, the VMA allocator and the descriptor layouts with every other
// Device through getVulkanShared().
//
// The queue is the one thing it does not own. A D3D12 command queue is created
// on demand and a Device makes its own; a VkQueue comes out of a family with a
// count the driver decides, and lavapipe - the device CI runs on - offers
// exactly one. So the queue is shared and every submit takes a mutex, and what
// keeps two Devices independent is everything else: separate pools, separate
// timelines, and a wait that names only its own submissions. nativeQueue() is
// therefore the same handle for every Device here, which is the one thing the
// Metal and D3D12 backends do differently.

namespace eacp::GPU
{
struct Device::Native
{
    // Mutable because the accessors that reach it are const - a const Device
    // still submits, the way a const Buffer still reads.
    mutable VulkanContext context;
};

Device::Device()
    : impl()
{
}

Device& Device::shared()
{
    static Device instance;

    // The process-wide Device is created lazily but belongs to the main thread
    // regardless of which thread asked for it first - every GPUView and every
    // Frame drives it from there. See VulkanContext::followMainThread.
    [[maybe_unused]] static const auto boundToMainThread =
        (instance.impl->context.followMainThread(), true);

    return instance;
}

VulkanContext& getVulkanContext(const Device& device)
{
    return *static_cast<VulkanContext*>(device.nativeContext());
}

bool Device::isValid() const
{
    return impl->context.isValid();
}

std::string Device::name() const
{
    if (!isValid())
        return "no Vulkan device";

    return getVulkanShared().getAdapterName();
}

// Straight off the device limits, and the intersection of the three a pass
// touches: a colour attachment, a depth attachment and a sampled image all have
// their own mask, and a count only one of them offers is a count a pass cannot
// be built at.
//
// Asked of the limits rather than per format, unlike D3D12: Vulkan's
// framebuffer masks are the guarantee for every format with the matching
// feature bits, and the per-format question (vkGetPhysicalDeviceImageFormat
// Properties) belongs with the Texture that would ask it - stage 3.
bool Device::supportsSampleCount(int count) const
{
    if (count <= 1)
        return true;

    if (!isValid() || count > 64 || (count & (count - 1)) != 0)
        return false;

    const auto& limits = getVulkanShared().getProperties().limits;
    const auto bit = static_cast<VkSampleCountFlags>(count);

    return (limits.framebufferColorSampleCounts & bit) != 0
           && (limits.framebufferDepthSampleCounts & bit) != 0
           && (limits.sampledImageColorSampleCounts & bit) != 0;
}

// A real query here, where D3D12's is literally isValid(): BC support is
// optional in Vulkan and Mesa's software rasterizer is one of the devices that
// has it, while a mobile driver reachable through the same loader may not.
bool Device::supportsBlockCompression() const
{
    return isValid()
           && getVulkanShared().getFeatures().textureCompressionBC == VK_TRUE;
}

void* Device::nativeContext() const
{
    return &impl->context;
}

void* Device::nativeDevice() const
{
    return getVulkanShared().getDevice();
}

void* Device::nativeQueue() const
{
    return getVulkanShared().getQueue();
}

void* Device::nativeTextureCache() const
{
    // No zero-copy pixel-buffer path on Linux: there is no camera or video
    // backend to produce one, and wrapPixelBuffer returns an invalid Texture.
    return nullptr;
}

void* Device::nativeSampler(TextureSampling) const
{
    // Nothing to hand back until there is a Texture to sample. A GLSL texture
    // binding is a combined image sampler, so the VkSampler travels with the
    // image in the descriptor write rather than being bound on its own - which
    // is where the four sampling configurations will be built (stage 3).
    return nullptr;
}

void Device::trackSubmittedWork(void*)
{
    // Nothing to record: every submit already signals this Device's timeline,
    // and lastSubmitted() is the value the wait below needs.
}

void Device::waitForSubmittedWork()
{
    impl->context.waitIdle();
}
} // namespace eacp::GPU
