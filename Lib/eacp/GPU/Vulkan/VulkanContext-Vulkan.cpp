#include "VulkanContext.h"

#include <eacp/Core/Utils/Logging.h>

#include <cstring>

namespace eacp::GPU
{
namespace
{
bool hasExtension(const Vector<VkExtensionProperties>& available, const char* name)
{
    for (const auto& extension: available)
        if (std::strcmp(extension.extensionName, name) == 0)
            return true;

    return false;
}

Vector<VkExtensionProperties> instanceExtensions()
{
    auto count = std::uint32_t {};
    vkEnumerateInstanceExtensionProperties(nullptr, &count, nullptr);

    auto result = Vector<VkExtensionProperties> {};
    result.resize(static_cast<int>(count));

    if (count > 0)
        vkEnumerateInstanceExtensionProperties(nullptr, &count, &result[0]);

    return result;
}

Vector<VkExtensionProperties> deviceExtensions(VkPhysicalDevice physicalDevice)
{
    auto count = std::uint32_t {};
    vkEnumerateDeviceExtensionProperties(physicalDevice, nullptr, &count, nullptr);

    auto result = Vector<VkExtensionProperties> {};
    result.resize(static_cast<int>(count));

    if (count > 0)
        vkEnumerateDeviceExtensionProperties(
            physicalDevice, nullptr, &count, &result[0]);

    return result;
}
} // namespace

VulkanContext::VulkanContext()
{
    if (!createInstance() || !pickPhysicalDevice() || !createDevice()
        || !createTimeline() || !createCommandPool())
    {
        destroyAll();
        LOG("Vulkan: no usable device; the GPU module will report invalid");
    }
}

VulkanContext::~VulkanContext()
{
    destroyAll();
}

bool VulkanContext::createInstance()
{
    auto available = instanceExtensions();

    auto names = Vector<const char*> {};

    // MoltenVK is a portability driver and the loader hides it unless the
    // instance opts in. Both are absent on a native driver, where the plain
    // enumeration already returns everything.
    auto portable = hasExtension(available, "VK_KHR_portability_enumeration");

    if (portable)
    {
        names.add("VK_KHR_portability_enumeration");
        names.add("VK_KHR_get_physical_device_properties2");
    }

    auto application = VkApplicationInfo {VK_STRUCTURE_TYPE_APPLICATION_INFO};
    application.pApplicationName = "eacp";
    application.apiVersion = VK_API_VERSION_1_2;

    auto info = VkInstanceCreateInfo {VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    info.pApplicationInfo = &application;
    info.enabledExtensionCount = static_cast<std::uint32_t>(names.size());
    info.ppEnabledExtensionNames = names.size() > 0 ? &names[0] : nullptr;

    if (portable)
        info.flags = VK_INSTANCE_CREATE_ENUMERATE_PORTABILITY_BIT_KHR;

    return vkCreateInstance(&info, nullptr, &instance) == VK_SUCCESS;
}

bool VulkanContext::pickPhysicalDevice()
{
    auto count = std::uint32_t {};
    vkEnumeratePhysicalDevices(instance, &count, nullptr);

    if (count == 0)
        return false;

    auto devices = Vector<VkPhysicalDevice> {};
    devices.resize(static_cast<int>(count));
    vkEnumeratePhysicalDevices(instance, &count, &devices[0]);

    // Prefer a discrete GPU, else take the first that offers graphics.
    auto chosen = VkPhysicalDevice {VK_NULL_HANDLE};

    for (const auto& candidate: devices)
    {
        auto properties = VkPhysicalDeviceProperties {};
        vkGetPhysicalDeviceProperties(candidate, &properties);

        if (chosen == VK_NULL_HANDLE
            || properties.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU)
            chosen = candidate;
    }

    if (chosen == VK_NULL_HANDLE)
        return false;

    physicalDevice = chosen;

    auto properties = VkPhysicalDeviceProperties {};
    vkGetPhysicalDeviceProperties(physicalDevice, &properties);
    pushConstantLimit = properties.limits.maxPushConstantsSize;

    vkGetPhysicalDeviceMemoryProperties(physicalDevice, &memoryProperties);

    auto families = std::uint32_t {};
    vkGetPhysicalDeviceQueueFamilyProperties(physicalDevice, &families, nullptr);

    auto queues = Vector<VkQueueFamilyProperties> {};
    queues.resize(static_cast<int>(families));
    vkGetPhysicalDeviceQueueFamilyProperties(physicalDevice, &families, &queues[0]);

    for (auto i = 0; i < queues.size(); ++i)
    {
        if ((queues[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) != 0)
        {
            queueFamily = static_cast<std::uint32_t>(i);
            return true;
        }
    }

    return false;
}

bool VulkanContext::createDevice()
{
    auto available = deviceExtensions(physicalDevice);

    auto names = Vector<const char*> {};
    names.add(VK_KHR_DYNAMIC_RENDERING_EXTENSION_NAME);

    // Required by the spec whenever the device advertises it, which is how a
    // portability driver like MoltenVK identifies itself.
    if (hasExtension(available, "VK_KHR_portability_subset"))
        names.add("VK_KHR_portability_subset");

    if (!hasExtension(available, VK_KHR_DYNAMIC_RENDERING_EXTENSION_NAME))
    {
        LOG("Vulkan: device lacks VK_KHR_dynamic_rendering");
        return false;
    }

    auto priority = 1.0f;
    auto queueInfo =
        VkDeviceQueueCreateInfo {VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    queueInfo.queueFamilyIndex = queueFamily;
    queueInfo.queueCount = 1;
    queueInfo.pQueuePriorities = &priority;

    auto dynamicRendering = VkPhysicalDeviceDynamicRenderingFeaturesKHR {
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DYNAMIC_RENDERING_FEATURES_KHR};
    dynamicRendering.dynamicRendering = VK_TRUE;

    auto vulkan12 = VkPhysicalDeviceVulkan12Features {
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
    vulkan12.timelineSemaphore = VK_TRUE;
    vulkan12.pNext = &dynamicRendering;

    auto features =
        VkPhysicalDeviceFeatures2 {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
    features.pNext = &vulkan12;

    auto info = VkDeviceCreateInfo {VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    info.pNext = &features;
    info.queueCreateInfoCount = 1;
    info.pQueueCreateInfos = &queueInfo;
    info.enabledExtensionCount = static_cast<std::uint32_t>(names.size());
    info.ppEnabledExtensionNames = &names[0];

    if (vkCreateDevice(physicalDevice, &info, nullptr, &device) != VK_SUCCESS)
        return false;

    vkGetDeviceQueue(device, queueFamily, 0, &queue);

    cmdBeginRendering = reinterpret_cast<PFN_vkCmdBeginRenderingKHR>(
        vkGetDeviceProcAddr(device, "vkCmdBeginRenderingKHR"));
    cmdEndRendering = reinterpret_cast<PFN_vkCmdEndRenderingKHR>(
        vkGetDeviceProcAddr(device, "vkCmdEndRenderingKHR"));

    return cmdBeginRendering != nullptr && cmdEndRendering != nullptr;
}

bool VulkanContext::createTimeline()
{
    auto type =
        VkSemaphoreTypeCreateInfo {VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO};
    type.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE;
    type.initialValue = 0;

    auto info = VkSemaphoreCreateInfo {VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    info.pNext = &type;

    return vkCreateSemaphore(device, &info, nullptr, &timeline) == VK_SUCCESS;
}

bool VulkanContext::createCommandPool()
{
    auto info = VkCommandPoolCreateInfo {VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    info.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    info.queueFamilyIndex = queueFamily;

    return vkCreateCommandPool(device, &info, nullptr, &commandPool) == VK_SUCCESS;
}

CommandContext* VulkanContext::acquire()
{
    if (!isValid())
        return nullptr;

    purgeRetired();

    auto* commands = static_cast<CommandContext*>(nullptr);

    for (auto i = 0; i < available.size(); ++i)
    {
        if (hasCompleted(available[i]->timelineValue))
        {
            commands = available[i];
            available.removeAt(i);
            break;
        }
    }

    if (commands == nullptr)
    {
        auto fresh = makeOwned<CommandContext>();

        auto info = VkCommandBufferAllocateInfo {
            VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        info.commandPool = commandPool;
        info.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        info.commandBufferCount = 1;

        if (vkAllocateCommandBuffers(device, &info, &fresh->list) != VK_SUCCESS)
            return nullptr;

        commands = fresh.get();
        pool.add(std::move(fresh));
    }

    for (auto i = 0; i < commands->transientBuffers.size(); ++i)
    {
        vkDestroyBuffer(device, commands->transientBuffers[i], nullptr);
        vkFreeMemory(device, commands->transientMemory[i], nullptr);
    }

    commands->transientBuffers.clear();
    commands->transientMemory.clear();
    commands->recordingId = ++recordingCounter;

    vkResetCommandBuffer(commands->list, 0);

    auto begin =
        VkCommandBufferBeginInfo {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(commands->list, &begin);

    return commands;
}

std::uint64_t VulkanContext::submit(CommandContext* commands)
{
    if (commands == nullptr || !isValid())
        return 0;

    vkEndCommandBuffer(commands->list);

    auto signalValue = nextTimelineValue++;

    auto timelineInfo = VkTimelineSemaphoreSubmitInfo {
        VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO};
    timelineInfo.signalSemaphoreValueCount = 1;
    timelineInfo.pSignalSemaphoreValues = &signalValue;

    auto info = VkSubmitInfo {VK_STRUCTURE_TYPE_SUBMIT_INFO};
    info.pNext = &timelineInfo;
    info.commandBufferCount = 1;
    info.pCommandBuffers = &commands->list;
    info.signalSemaphoreCount = 1;
    info.pSignalSemaphores = &timeline;

    if (vkQueueSubmit(queue, 1, &info, VK_NULL_HANDLE) != VK_SUCCESS)
    {
        discard(commands);
        return 0;
    }

    commands->timelineValue = signalValue;
    lastSubmittedValue = signalValue;
    available.add(commands);

    return signalValue;
}

void VulkanContext::discard(CommandContext* commands)
{
    if (commands == nullptr)
        return;

    vkEndCommandBuffer(commands->list);
    commands->timelineValue = 0;
    available.add(commands);
}

bool VulkanContext::hasCompleted(std::uint64_t value) const
{
    if (value == 0)
        return true;

    if (!isValid())
        return true;

    auto current = std::uint64_t {};
    vkGetSemaphoreCounterValue(device, timeline, &current);

    return current >= value;
}

void VulkanContext::waitFor(std::uint64_t value)
{
    if (value == 0 || !isValid() || hasCompleted(value))
        return;

    auto info = VkSemaphoreWaitInfo {VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO};
    info.semaphoreCount = 1;
    info.pSemaphores = &timeline;
    info.pValues = &value;

    vkWaitSemaphores(device, &info, UINT64_MAX);
}

void VulkanContext::waitIdle()
{
    if (isValid())
        vkDeviceWaitIdle(device);

    purgeRetired();
}

std::uint32_t VulkanContext::findMemoryType(std::uint32_t typeBits,
                                            VkMemoryPropertyFlags properties) const
{
    for (auto i = std::uint32_t {}; i < memoryProperties.memoryTypeCount; ++i)
    {
        auto matchesType = (typeBits & (1u << i)) != 0;
        auto flags = memoryProperties.memoryTypes[i].propertyFlags;

        if (matchesType && (flags & properties) == properties)
            return i;
    }

    return UINT32_MAX;
}

Allocation VulkanContext::allocateFor(VkBuffer buffer,
                                      VkMemoryPropertyFlags properties)
{
    auto requirements = VkMemoryRequirements {};
    vkGetBufferMemoryRequirements(device, buffer, &requirements);

    auto type = findMemoryType(requirements.memoryTypeBits, properties);

    if (type == UINT32_MAX)
        return {};

    auto info = VkMemoryAllocateInfo {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    info.allocationSize = requirements.size;
    info.memoryTypeIndex = type;

    auto result = Allocation {};

    if (vkAllocateMemory(device, &info, nullptr, &result.memory) != VK_SUCCESS)
        return {};

    result.bytes = requirements.size;
    vkBindBufferMemory(device, buffer, result.memory, 0);

    return result;
}

Allocation VulkanContext::allocateFor(VkImage image,
                                      VkMemoryPropertyFlags properties)
{
    auto requirements = VkMemoryRequirements {};
    vkGetImageMemoryRequirements(device, image, &requirements);

    auto type = findMemoryType(requirements.memoryTypeBits, properties);

    if (type == UINT32_MAX)
        return {};

    auto info = VkMemoryAllocateInfo {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    info.allocationSize = requirements.size;
    info.memoryTypeIndex = type;

    auto result = Allocation {};

    if (vkAllocateMemory(device, &info, nullptr, &result.memory) != VK_SUCCESS)
        return {};

    result.bytes = requirements.size;
    vkBindImageMemory(device, image, result.memory, 0);

    return result;
}

VkBuffer VulkanContext::makeStagingBuffer(CommandContext& commands,
                                          const void* data,
                                          std::size_t bytes)
{
    if (bytes == 0)
        return VK_NULL_HANDLE;

    auto info = VkBufferCreateInfo {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    info.size = bytes;
    info.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

    auto buffer = VkBuffer {VK_NULL_HANDLE};

    if (vkCreateBuffer(device, &info, nullptr, &buffer) != VK_SUCCESS)
        return VK_NULL_HANDLE;

    auto allocation = allocateFor(buffer,
                                  VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT
                                      | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);

    if (allocation.memory == VK_NULL_HANDLE)
    {
        vkDestroyBuffer(device, buffer, nullptr);
        return VK_NULL_HANDLE;
    }

    if (data != nullptr)
    {
        auto* mapped = static_cast<void*>(nullptr);

        if (vkMapMemory(device, allocation.memory, 0, bytes, 0, &mapped)
            == VK_SUCCESS)
        {
            std::memcpy(mapped, data, bytes);
            vkUnmapMemory(device, allocation.memory);
        }
    }

    commands.transientBuffers.add(buffer);
    commands.transientMemory.add(allocation.memory);

    return buffer;
}

void VulkanContext::deferDestroy(VkBuffer buffer, VkDeviceMemory memory)
{
    if (buffer == VK_NULL_HANDLE && memory == VK_NULL_HANDLE)
        return;

    auto entry = Retired {};
    entry.buffer = buffer;
    entry.memory = memory;
    entry.timelineValue = lastSubmittedValue;
    retired.add(entry);
}

void VulkanContext::deferDestroy(VkImage image,
                                 VkImageView view,
                                 VkDeviceMemory memory)
{
    if (image == VK_NULL_HANDLE && view == VK_NULL_HANDLE
        && memory == VK_NULL_HANDLE)
        return;

    auto entry = Retired {};
    entry.image = image;
    entry.view = view;
    entry.memory = memory;
    entry.timelineValue = lastSubmittedValue;
    retired.add(entry);
}

void VulkanContext::deferDestroy(VkPipeline pipeline)
{
    if (pipeline == VK_NULL_HANDLE)
        return;

    auto entry = Retired {};
    entry.pipeline = pipeline;
    entry.timelineValue = lastSubmittedValue;
    retired.add(entry);
}

void VulkanContext::purgeRetired()
{
    for (auto i = retired.size() - 1; i >= 0; --i)
    {
        if (!hasCompleted(retired[i].timelineValue))
            continue;

        const auto& entry = retired[i];

        if (entry.view != VK_NULL_HANDLE)
            vkDestroyImageView(device, entry.view, nullptr);

        if (entry.image != VK_NULL_HANDLE)
            vkDestroyImage(device, entry.image, nullptr);

        if (entry.buffer != VK_NULL_HANDLE)
            vkDestroyBuffer(device, entry.buffer, nullptr);

        if (entry.pipeline != VK_NULL_HANDLE)
            vkDestroyPipeline(device, entry.pipeline, nullptr);

        if (entry.memory != VK_NULL_HANDLE)
            vkFreeMemory(device, entry.memory, nullptr);

        retired.removeAt(i);
    }
}

void VulkanContext::beginRendering(VkCommandBuffer list,
                                   const VkRenderingInfoKHR& info) const
{
    cmdBeginRendering(list, &info);
}

void VulkanContext::endRendering(VkCommandBuffer list) const
{
    cmdEndRendering(list);
}

void VulkanContext::destroyAll()
{
    if (device != VK_NULL_HANDLE)
    {
        vkDeviceWaitIdle(device);

        for (auto& commands: pool)
        {
            for (auto i = 0; i < commands->transientBuffers.size(); ++i)
            {
                vkDestroyBuffer(device, commands->transientBuffers[i], nullptr);
                vkFreeMemory(device, commands->transientMemory[i], nullptr);
            }
        }

        for (const auto& entry: retired)
        {
            if (entry.view != VK_NULL_HANDLE)
                vkDestroyImageView(device, entry.view, nullptr);

            if (entry.image != VK_NULL_HANDLE)
                vkDestroyImage(device, entry.image, nullptr);

            if (entry.buffer != VK_NULL_HANDLE)
                vkDestroyBuffer(device, entry.buffer, nullptr);

            if (entry.pipeline != VK_NULL_HANDLE)
                vkDestroyPipeline(device, entry.pipeline, nullptr);

            if (entry.memory != VK_NULL_HANDLE)
                vkFreeMemory(device, entry.memory, nullptr);
        }

        retired.clear();
        pool.clear();
        available.clear();

        if (commandPool != VK_NULL_HANDLE)
            vkDestroyCommandPool(device, commandPool, nullptr);

        if (timeline != VK_NULL_HANDLE)
            vkDestroySemaphore(device, timeline, nullptr);

        vkDestroyDevice(device, nullptr);
    }

    if (instance != VK_NULL_HANDLE)
        vkDestroyInstance(instance, nullptr);

    commandPool = VK_NULL_HANDLE;
    timeline = VK_NULL_HANDLE;
    device = VK_NULL_HANDLE;
    physicalDevice = VK_NULL_HANDLE;
    queue = VK_NULL_HANDLE;
    instance = VK_NULL_HANDLE;
}

VulkanContext& getVulkanContext()
{
    static VulkanContext context;
    return context;
}
} // namespace eacp::GPU
