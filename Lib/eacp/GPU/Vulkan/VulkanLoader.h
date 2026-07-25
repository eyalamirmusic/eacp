#pragma once

#include <vulkan/vulkan.h>

// Every Vulkan entry point the backend calls, as a function pointer resolved at
// runtime rather than a symbol resolved at link time. eacp links no import
// library, which buys two things: the build needs the Khronos headers and
// nothing else (no SDK on any platform -- see FindVulkanHeaders.cmake), and an
// app still starts on a machine with no Vulkan driver, where getVulkanContext()
// simply reports invalid instead of the process failing to load at all.
//
// The names live in eacp::GPU, so the unqualified vkCreateBuffer(...) calls
// throughout the backend find these by ordinary lookup. vulkan.h declares no
// competing prototypes because VK_NO_PROTOTYPES is set on the target, which is
// where it belongs -- a define in a header only works if every translation unit
// reaches that header first.
//
// Three tiers, because that is how Vulkan's own dispatch is layered: global
// functions exist before an instance does, instance functions come from the
// instance, and device functions are fetched through vkGetDeviceProcAddr so a
// call goes straight to the driver instead of through the loader's trampoline.

namespace eacp::GPU
{
#define EACP_VULKAN_GLOBAL_FUNCTIONS(X)                                             \
    X(vkCreateInstance)                                                             \
    X(vkEnumerateInstanceExtensionProperties)

#define EACP_VULKAN_INSTANCE_FUNCTIONS(X)                                           \
    X(vkCreateDevice)                                                               \
    X(vkDestroyInstance)                                                            \
    X(vkDestroySurfaceKHR)                                                          \
    X(vkEnumerateDeviceExtensionProperties)                                         \
    X(vkEnumeratePhysicalDevices)                                                   \
    X(vkGetDeviceProcAddr)                                                          \
    X(vkGetPhysicalDeviceMemoryProperties)                                          \
    X(vkGetPhysicalDeviceProperties)                                                \
    X(vkGetPhysicalDeviceQueueFamilyProperties)                                     \
    X(vkGetPhysicalDeviceSurfaceCapabilitiesKHR)

#define EACP_VULKAN_DEVICE_FUNCTIONS(X)                                             \
    X(vkAcquireNextImageKHR)                                                        \
    X(vkAllocateCommandBuffers)                                                     \
    X(vkAllocateDescriptorSets)                                                     \
    X(vkAllocateMemory)                                                             \
    X(vkBeginCommandBuffer)                                                         \
    X(vkBindBufferMemory)                                                           \
    X(vkBindImageMemory)                                                            \
    X(vkCmdBindDescriptorSets)                                                      \
    X(vkCmdBindIndexBuffer)                                                         \
    X(vkCmdBindPipeline)                                                            \
    X(vkCmdBindVertexBuffers)                                                       \
    X(vkCmdCopyBufferToImage)                                                       \
    X(vkCmdCopyImageToBuffer)                                                       \
    X(vkCmdDispatch)                                                                \
    X(vkCmdDraw)                                                                    \
    X(vkCmdDrawIndexed)                                                             \
    X(vkCmdPipelineBarrier)                                                         \
    X(vkCmdPushConstants)                                                           \
    X(vkCmdSetScissor)                                                              \
    X(vkCmdSetViewport)                                                             \
    X(vkCreateBuffer)                                                               \
    X(vkCreateCommandPool)                                                          \
    X(vkCreateComputePipelines)                                                     \
    X(vkCreateDescriptorPool)                                                       \
    X(vkCreateDescriptorSetLayout)                                                  \
    X(vkCreateGraphicsPipelines)                                                    \
    X(vkCreateImage)                                                                \
    X(vkCreateImageView)                                                            \
    X(vkCreatePipelineLayout)                                                       \
    X(vkCreateSampler)                                                              \
    X(vkCreateSemaphore)                                                            \
    X(vkCreateShaderModule)                                                         \
    X(vkCreateSwapchainKHR)                                                         \
    X(vkDestroyBuffer)                                                              \
    X(vkDestroyCommandPool)                                                         \
    X(vkDestroyDescriptorPool)                                                      \
    X(vkDestroyDescriptorSetLayout)                                                 \
    X(vkDestroyDevice)                                                              \
    X(vkDestroyImage)                                                               \
    X(vkDestroyImageView)                                                           \
    X(vkDestroyPipeline)                                                            \
    X(vkDestroyPipelineLayout)                                                      \
    X(vkDestroySampler)                                                             \
    X(vkDestroySemaphore)                                                           \
    X(vkDestroyShaderModule)                                                        \
    X(vkDestroySwapchainKHR)                                                        \
    X(vkDeviceWaitIdle)                                                             \
    X(vkEndCommandBuffer)                                                           \
    X(vkFreeDescriptorSets)                                                         \
    X(vkFreeMemory)                                                                 \
    X(vkGetBufferMemoryRequirements)                                                \
    X(vkGetDeviceQueue)                                                             \
    X(vkGetImageMemoryRequirements)                                                 \
    X(vkGetSemaphoreCounterValue)                                                   \
    X(vkGetSwapchainImagesKHR)                                                      \
    X(vkMapMemory)                                                                  \
    X(vkQueuePresentKHR)                                                            \
    X(vkQueueSubmit)                                                                \
    X(vkResetCommandBuffer)                                                         \
    X(vkUnmapMemory)                                                                \
    X(vkUpdateDescriptorSets)                                                       \
    X(vkWaitSemaphores)

#define EACP_VULKAN_DECLARE_FUNCTION(name) extern PFN_##name name;

EACP_VULKAN_GLOBAL_FUNCTIONS(EACP_VULKAN_DECLARE_FUNCTION)
EACP_VULKAN_INSTANCE_FUNCTIONS(EACP_VULKAN_DECLARE_FUNCTION)
EACP_VULKAN_DEVICE_FUNCTIONS(EACP_VULKAN_DECLARE_FUNCTION)

#undef EACP_VULKAN_DECLARE_FUNCTION

// The bootstrap the other three tiers are resolved through. Also what a platform
// surface extension is fetched with (vkCreateMetalSurfaceEXT,
// vkCreateWin32SurfaceKHR), since those are known only to the file that uses one.
extern PFN_vkGetInstanceProcAddr vkGetInstanceProcAddr;

// Opens the platform's Vulkan library and resolves the global tier. False when
// the machine has no Vulkan at all, which is an ordinary outcome rather than an
// error: the caller reports an invalid device and the GPU module stands down.
// Repeat calls are cheap, and the library stays loaded for the process lifetime.
bool loadVulkanGlobals();

void loadVulkanInstance(VkInstance instance);
void loadVulkanDevice(VkDevice device);

namespace detail
{
// The one piece of this that cannot be portable: which library holds Vulkan and
// how it is opened. vulkan-1.dll on Windows, libvulkan or MoltenVK on Apple, and
// on iOS the running image itself, where MoltenVK is linked in rather than
// loaded. Null when there is none.
PFN_vkGetInstanceProcAddr vulkanEntryPoint();
} // namespace detail
} // namespace eacp::GPU
