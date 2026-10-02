#include "Device.h"

#include <algorithm>

#include "../Vulkan/VulkanContext.h"

namespace eacp::GPU
{
struct Device::Native
{
    // Mutable because the accessors that reach it are const, and a const Device
    // still submits.
    mutable VulkanContext context;
};

Device::Device()
    : impl()
{
}

Device& Device::shared()
{
    static Device instance;

    // Belongs to the main thread whichever thread asked for it first.
    [[maybe_unused]] static const auto boundToMainThread =
        (instance.followMainThread(),
         instance.impl->context.followMainThread(),
         true);

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

// The intersection of the three masks a pass touches.
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

// BC support is optional in Vulkan.
bool Device::supportsBlockCompression() const
{
    return isValid()
           && getVulkanShared().getFeatures().textureCompressionBC == VK_TRUE;
}

// The one range rule that is a device property rather than a constant: a
// descriptor may name no offset off minStorageBufferOffsetAlignment.
int Device::storageBufferOffsetAlignment() const
{
    if (!isValid())
        return 4;

    const auto alignment =
        getVulkanShared().getProperties().limits.minStorageBufferOffsetAlignment;

    return alignment > 0 ? (int) alignment : 4;
}

int Device::maxThreadgroupMemory() const
{
    if (!isValid())
        return 0;

    return (int) getVulkanShared().getProperties().limits.maxComputeSharedMemorySize;
}

// The largest device-local heap, which is what a discrete card's memory is and
// what a software or integrated driver reports as a slice of host RAM -
// lavapipe answers system memory, and treating that as the budget is right,
// since that is exactly where its "device" memory comes from. Vulkan has no
// per-process recommendation to ask for, so the heap size is the honest
// answer; zero where there is no device.
std::int64_t Device::memoryBudget() const
{
    if (!isValid())
        return 0;

    auto memory = VkPhysicalDeviceMemoryProperties {};
    vkGetPhysicalDeviceMemoryProperties(getVulkanShared().getPhysicalDevice(),
                                        &memory);

    auto largest = std::int64_t {0};

    for (auto i = 0u; i < memory.memoryHeapCount; ++i)
    {
        const auto& heap = memory.memoryHeaps[i];

        if ((heap.flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) != 0)
            largest = std::max(largest, (std::int64_t) heap.size);
    }

    return largest;
}

// No, and not because of the hardware: eacp emits GLSL 450 with no
// cooperative-matrix extension, so a fragment here is the two-floats-per-lane
// emulation whatever the driver could have done. The packed loads work on it -
// each lane unpacks the pair it holds - and are simply not faster, so there is
// nothing for a kernel to restructure itself around.
bool Device::supportsHalfSimdMatrix() const
{
    return false;
}

bool Device::supportsBFloat16SimdMatrix() const
{
    return false;
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
    // No zero-copy pixel-buffer path on Linux.
    return nullptr;
}

// For callers outside the backend: the bind sites take the sampler straight off
// VulkanShared.
void* Device::nativeSampler(TextureSampling sampling) const
{
    if (!isValid())
        return nullptr;

    return getVulkanShared().getSampler(sampling);
}

void Device::trackSubmittedWork(void*)
{
    // Nothing to record: every submit already signals this Device's timeline.
}

void Device::waitForSubmittedWork()
{
    impl->context.waitIdle();
}

// The timeline semaphore's values are the serials: every submission signals
// the next one, and its counter is how far the GPU has got.
std::uint64_t Device::lastSubmission() const
{
    return impl->context.lastSubmitted();
}

bool Device::hasFinished(std::uint64_t submission) const
{
    return impl->context.hasCompleted(submission);
}
} // namespace eacp::GPU
