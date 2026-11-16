#pragma once

#include "../Device/AdapterDescription.h"

#include <volk.h>

#include <cstdint>
#include <string>

namespace eacp::GPU
{
AdapterDescription
    describeVulkanDevice(VkPhysicalDevice physical,
                         const VkPhysicalDeviceProperties& forProperties);

void logFoundDevice(VkPhysicalDevice physical,
                    const VkPhysicalDeviceProperties& forProperties);
} // namespace eacp::GPU
