#include "VulkanLoader.h"

namespace eacp::GPU
{
#define EACP_VULKAN_DEFINE_FUNCTION(name) PFN_##name name = nullptr;

EACP_VULKAN_GLOBAL_FUNCTIONS(EACP_VULKAN_DEFINE_FUNCTION)
EACP_VULKAN_INSTANCE_FUNCTIONS(EACP_VULKAN_DEFINE_FUNCTION)
EACP_VULKAN_DEVICE_FUNCTIONS(EACP_VULKAN_DEFINE_FUNCTION)

#undef EACP_VULKAN_DEFINE_FUNCTION

PFN_vkGetInstanceProcAddr vkGetInstanceProcAddr = nullptr;

bool loadVulkanGlobals()
{
    if (vkCreateInstance != nullptr)
        return true;

    vkGetInstanceProcAddr = detail::vulkanEntryPoint();

    if (vkGetInstanceProcAddr == nullptr)
        return false;

#define EACP_VULKAN_LOAD_GLOBAL(name)                                               \
    name =                                                                          \
        reinterpret_cast<PFN_##name>(vkGetInstanceProcAddr(VK_NULL_HANDLE, #name));

    EACP_VULKAN_GLOBAL_FUNCTIONS(EACP_VULKAN_LOAD_GLOBAL)

#undef EACP_VULKAN_LOAD_GLOBAL

    // A loader with no driver behind it still answers for the global tier, so
    // this only rules out a library that is not a Vulkan loader at all. Whether
    // there is anything to render with is vkCreateInstance's answer to give.
    return vkCreateInstance != nullptr;
}

void loadVulkanInstance(VkInstance instance)
{
#define EACP_VULKAN_LOAD_INSTANCE(name)                                             \
    name = reinterpret_cast<PFN_##name>(vkGetInstanceProcAddr(instance, #name));

    EACP_VULKAN_INSTANCE_FUNCTIONS(EACP_VULKAN_LOAD_INSTANCE)

#undef EACP_VULKAN_LOAD_INSTANCE
}

void loadVulkanDevice(VkDevice device)
{
    // Through the device rather than the instance: an instance-resolved device
    // function is a trampoline that looks the device up on every call, and these
    // are the ones a frame goes through thousands of times.
#define EACP_VULKAN_LOAD_DEVICE(name)                                               \
    name = reinterpret_cast<PFN_##name>(vkGetDeviceProcAddr(device, #name));

    EACP_VULKAN_DEVICE_FUNCTIONS(EACP_VULKAN_LOAD_DEVICE)

#undef EACP_VULKAN_LOAD_DEVICE
}
} // namespace eacp::GPU
