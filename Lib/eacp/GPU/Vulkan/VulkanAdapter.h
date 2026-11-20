#pragma once

#include "../Device/AdapterDescription.h"

#include <volk.h>

#include <cstdint>
#include <string>

namespace eacp::GPU
{
// "1.3": the major and minor parts of a packed VK_API_VERSION.
std::string apiVersionText(std::uint32_t version);

AdapterDescription
    describeVulkanDevice(VkPhysicalDevice physical,
                         const VkPhysicalDeviceProperties& forProperties);

void logFoundDevice(VkPhysicalDevice physical,
                    const VkPhysicalDeviceProperties& forProperties);
} // namespace eacp::GPU
