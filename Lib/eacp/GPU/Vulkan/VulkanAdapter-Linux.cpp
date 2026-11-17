#include "VulkanAdapter.h"
#include "VulkanContext.h"

#include <eacp/Core/Utils/Logging.h>

namespace eacp::GPU
{
namespace
{
constexpr auto nvidiaVendorId = std::uint32_t {0x10DE};

// Vulkan leaves driverVersion's packing to the vendor: NVIDIA packs 10.8.8.6
// bits, Mesa and most others pack it as an API version.
std::string decodeDriverVersion(const VkPhysicalDeviceProperties& forProperties)
{
    const auto version = forProperties.driverVersion;

    if (forProperties.vendorID != nvidiaVendorId)
    {
        return std::to_string(VK_API_VERSION_MAJOR(version)) + "."
               + std::to_string(VK_API_VERSION_MINOR(version)) + "."
               + std::to_string(VK_API_VERSION_PATCH(version));
    }

    constexpr auto majorShift = 22;
    constexpr auto minorShift = 14;
    constexpr auto buildShift = 6;
    constexpr auto majorMask = 0x3FFu;
    constexpr auto minorAndBuildMask = 0xFFu;
    constexpr auto patchMask = 0x3Fu;

    return Strings::format("{}.{}.{}.{}",
                           (version >> majorShift) & majorMask,
                           (version >> minorShift) & minorAndBuildMask,
                           (version >> buildShift) & minorAndBuildMask,
                           version & patchMask);
}

// Counted only on a discrete GPU: an integrated or software device's
// device-local heap is system memory, which DXGI does not call dedicated either.
std::uint64_t sumDedicatedMemory(VkPhysicalDevice physical,
                                 const VkPhysicalDeviceProperties& forProperties)
{
    if (forProperties.deviceType != VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU)
        return 0;

    VkPhysicalDeviceMemoryProperties memory = {};
    vkGetPhysicalDeviceMemoryProperties(physical, &memory);

    auto total = std::uint64_t {0};

    for (auto index = std::uint32_t {0}; index < memory.memoryHeapCount; ++index)
    {
        if ((memory.memoryHeaps[index].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) != 0)
            total += memory.memoryHeaps[index].size;
    }

    return total;
}
} // namespace

std::string apiVersionText(std::uint32_t version)
{
    return std::to_string(VK_API_VERSION_MAJOR(version)) + "."
           + std::to_string(VK_API_VERSION_MINOR(version));
}

AdapterDescription
    describeVulkanDevice(VkPhysicalDevice physical,
                         const VkPhysicalDeviceProperties& forProperties)
{
    return {.name = forProperties.deviceName,
            .vendorId = forProperties.vendorID,
            .deviceId = forProperties.deviceID,
            .dedicatedMemoryBytes = sumDedicatedMemory(physical, forProperties),
            .driverVersion = decodeDriverVersion(forProperties),
            .apiLevel = "Vulkan " + apiVersionText(forProperties.apiVersion),
            .isSoftware = forProperties.deviceType == VK_PHYSICAL_DEVICE_TYPE_CPU};
}

void logFoundDevice(VkPhysicalDevice physical,
                    const VkPhysicalDeviceProperties& forProperties)
{
    LOG("Vulkan: found device: ",
        describeAdapterForLog(describeVulkanDevice(physical, forProperties)));
}
} // namespace eacp::GPU
