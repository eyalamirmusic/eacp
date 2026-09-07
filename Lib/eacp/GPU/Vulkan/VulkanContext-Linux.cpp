#include "../Common.h"

#include "VulkanContext.h"
#include "VulkanTypes.h"

#include "../Codegen/UniformLayout.h"
#include "../Spirv/SpirvCompiler.h"

#include <eacp/Core/Threads/ThreadUtils.h>
#include <eacp/Core/Utils/Environment.h>

#include <algorithm>
#include <cassert>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <thread>

#include <unistd.h>

namespace eacp::GPU
{
namespace
{
// Sized so a recording of a few hundred dispatches never allocates mid-frame.
constexpr std::size_t vulkanConstantPageBytes = 64 * 1024;

// A frame uses well under a megabyte, so one chunk is made and refilled.
constexpr std::size_t vulkanUploadChunkBytes = 1024 * 1024;

// A dispatch takes one set: dispatches per recording before a second pool.
constexpr std::uint32_t vulkanSetsPerDescriptorPool = 64;

// A descriptor range shorter than the block the shader declares is a validation
// error, and the CPU packs the block to its widest member instead.
constexpr auto vulkanUniformBlockRounding =
    static_cast<std::size_t>(std140BlockAlignment);

std::size_t roundUpTo(std::size_t value, std::size_t alignment)
{
    return alignment == 0 ? value : (value + alignment - 1) / alignment * alignment;
}

// Whether an EACP_VK_* environment switch is set to anything but "0".
bool vulkanEnvironmentFlag(const char* name)
{
    const auto value = getEnvValue(name);
    return !value.empty() && value != "0";
}

// EACP_VK_SOFTWARE=1 takes a CPU device over the hardware one.
bool prefersSoftwareDevice()
{
    return vulkanEnvironmentFlag("EACP_VK_SOFTWARE");
}

// EACP_VK_VALIDATION=1 turns on the validation layer and a logging messenger.
bool wantsValidation()
{
    return vulkanEnvironmentFlag("EACP_VK_VALIDATION");
}

std::uint64_t currentThreadId()
{
    return static_cast<std::uint64_t>(
        std::hash<std::thread::id> {}(std::this_thread::get_id()));
}

// $XDG_CACHE_HOME/eacp, and $HOME/.cache/eacp where the first is unset. Empty
// when neither is, which turns the pipeline cache off rather than guessing.
std::string vulkanCacheDirectory()
{
    const auto xdg = getEnvValue("XDG_CACHE_HOME");

    if (!xdg.empty())
        return xdg + "/eacp";

    const auto home = getEnvValue("HOME");

    if (home.empty())
        return {};

    return home + "/.cache/eacp";
}

std::string toHex(const std::uint8_t* bytes, int count)
{
    constexpr auto digits = "0123456789abcdef";

    auto text = std::string {};

    for (auto i = 0; i < count; ++i)
    {
        text += digits[bytes[i] >> 4];
        text += digits[bytes[i] & 0xf];
    }

    return text;
}

// Keyed by the driver's own cache UUID, so a driver update writes a new file
// rather than fighting over one.
std::string vulkanPipelineCachePath(const VkPhysicalDeviceProperties& forProperties)
{
    const auto directory = vulkanCacheDirectory();

    if (directory.empty())
        return {};

    return directory + "/pipelines-"
           + toHex(forProperties.pipelineCacheUUID, VK_UUID_SIZE) + ".bin";
}

Vector<std::byte> readFileBytes(const std::string& path)
{
    auto blob = Vector<std::byte> {};

    if (path.empty())
        return blob;

    auto in = std::ifstream {path, std::ios::binary | std::ios::ate};

    if (!in.is_open())
        return blob;

    const auto size = static_cast<long long>(in.tellg());

    if (size <= 0)
        return blob;

    blob.resize(static_cast<int>(size));
    in.seekg(0);
    in.read(reinterpret_cast<char*>(blob.data()),
            static_cast<std::streamsize>(size));

    if (!in.good())
        blob.clear();

    return blob;
}

// Written beside the target and renamed onto it, so a reader never sees half a
// cache - and named for this process, so two of them do not share the temporary.
void writeFileAtomically(const std::string& path, const Vector<std::byte>& blob)
{
    if (path.empty() || blob.empty())
        return;

    auto error = std::error_code {};
    std::filesystem::create_directories(std::filesystem::path {path}.parent_path(),
                                        error);

    const auto temporary = path + "." + std::to_string(getpid()) + ".tmp";

    {
        auto out = std::ofstream {temporary, std::ios::binary | std::ios::trunc};

        if (!out.is_open())
            return;

        out.write(reinterpret_cast<const char*>(blob.data()),
                  static_cast<std::streamsize>(blob.size()));
        out.close();

        if (!out.good())
        {
            std::filesystem::remove(temporary, error);
            return;
        }
    }

    std::filesystem::rename(temporary, path, error);

    if (error)
        std::filesystem::remove(temporary, error);
}

// A cache written by another device, or by a driver that has since been
// updated, is data this one cannot read: the header carries what it was for.
bool pipelineCacheHeaderMatches(const Vector<std::byte>& blob,
                                const VkPhysicalDeviceProperties& forProperties)
{
    VkPipelineCacheHeaderVersionOne header = {};

    if (blob.size() < static_cast<int>(sizeof(header)))
        return false;

    std::memcpy(&header, blob.data(), sizeof(header));

    return header.headerSize == sizeof(header)
           && header.headerVersion == VK_PIPELINE_CACHE_HEADER_VERSION_ONE
           && header.vendorID == forProperties.vendorID
           && header.deviceID == forProperties.deviceID
           && std::memcmp(header.pipelineCacheUUID,
                          forProperties.pipelineCacheUUID,
                          VK_UUID_SIZE)
                  == 0;
}

// The spec requires sample zero in both masks; this only catches a driver that
// offers no depth resolve at all.
bool queryDepthResolvesBySampleZero(VkPhysicalDevice physical)
{
    VkPhysicalDeviceDepthStencilResolveProperties resolve = {};
    resolve.sType =
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DEPTH_STENCIL_RESOLVE_PROPERTIES;

    VkPhysicalDeviceProperties2 all = {};
    all.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
    all.pNext = &resolve;

    vkGetPhysicalDeviceProperties2(physical, &all);

    const auto sampleZero = VkResolveModeFlags {VK_RESOLVE_MODE_SAMPLE_ZERO_BIT};

    return (resolve.supportedDepthResolveModes & sampleZero) != 0
           && (resolve.supportedStencilResolveModes & sampleZero) != 0;
}

int deviceRank(VkPhysicalDeviceType type)
{
    switch (type)
    {
        case VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU:
            return 4;
        case VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU:
            return 3;
        case VK_PHYSICAL_DEVICE_TYPE_VIRTUAL_GPU:
            return 2;
        case VK_PHYSICAL_DEVICE_TYPE_CPU:
            return 1;
        default:
            return 0;
    }
}

bool hasInstanceLayer(const char* name)
{
    auto count = std::uint32_t {0};
    vkEnumerateInstanceLayerProperties(&count, nullptr);

    auto layers = Vector<VkLayerProperties> {};
    layers.resize(static_cast<int>(count));
    vkEnumerateInstanceLayerProperties(&count, layers.data());

    for (const auto& layer: layers)
        if (std::strcmp(layer.layerName, name) == 0)
            return true;

    return false;
}

bool hasInstanceExtension(const char* name)
{
    auto count = std::uint32_t {0};
    vkEnumerateInstanceExtensionProperties(nullptr, &count, nullptr);

    auto extensions = Vector<VkExtensionProperties> {};
    extensions.resize(static_cast<int>(count));
    vkEnumerateInstanceExtensionProperties(nullptr, &count, extensions.data());

    for (const auto& extension: extensions)
        if (std::strcmp(extension.extensionName, name) == 0)
            return true;

    return false;
}

bool hasDeviceExtension(VkPhysicalDevice candidate, const char* name)
{
    auto count = std::uint32_t {0};
    vkEnumerateDeviceExtensionProperties(candidate, nullptr, &count, nullptr);

    auto extensions = Vector<VkExtensionProperties> {};
    extensions.resize(static_cast<int>(count));
    vkEnumerateDeviceExtensionProperties(
        candidate, nullptr, &count, extensions.data());

    for (const auto& extension: extensions)
        if (std::strcmp(extension.extensionName, name) == 0)
            return true;

    return false;
}

VKAPI_ATTR VkBool32 VKAPI_CALL
    vulkanDebugCallback(VkDebugUtilsMessageSeverityFlagBitsEXT,
                        VkDebugUtilsMessageTypeFlagsEXT,
                        const VkDebugUtilsMessengerCallbackDataEXT* data,
                        void*)
{
    if (data != nullptr && data->pMessage != nullptr)
        LOG("Vulkan: ", data->pMessage);

    // True would abort the call being validated.
    return VK_FALSE;
}

// The floor the backend is written against, on top of Vulkan 1.3 core.
struct RequiredFeatures
{
    bool timelineSemaphore = false;
    bool descriptorBindingPartiallyBound = false;
    bool synchronization2 = false;
    bool dynamicRendering = false;
    bool shaderStorageImageWriteWithoutFormat = false;

    bool allPresent() const
    {
        return timelineSemaphore && descriptorBindingPartiallyBound
               && synchronization2 && dynamicRendering
               && shaderStorageImageWriteWithoutFormat;
    }
};

RequiredFeatures probeFeatures(VkPhysicalDevice candidate)
{
    VkPhysicalDeviceVulkan13Features features13 = {};
    features13.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES;

    VkPhysicalDeviceVulkan12Features features12 = {};
    features12.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES;
    features12.pNext = &features13;

    VkPhysicalDeviceFeatures2 features = {};
    features.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
    features.pNext = &features12;

    vkGetPhysicalDeviceFeatures2(candidate, &features);

    auto required = RequiredFeatures {};
    required.timelineSemaphore = features12.timelineSemaphore == VK_TRUE;
    required.descriptorBindingPartiallyBound =
        features12.descriptorBindingPartiallyBound == VK_TRUE;
    required.synchronization2 = features13.synchronization2 == VK_TRUE;
    required.dynamicRendering = features13.dynamicRendering == VK_TRUE;
    required.shaderStorageImageWriteWithoutFormat =
        features.features.shaderStorageImageWriteWithoutFormat == VK_TRUE;

    return required;
}

int findQueueFamily(VkPhysicalDevice candidate)
{
    auto count = std::uint32_t {0};
    vkGetPhysicalDeviceQueueFamilyProperties(candidate, &count, nullptr);

    auto families = Vector<VkQueueFamilyProperties> {};
    families.resize(static_cast<int>(count));
    vkGetPhysicalDeviceQueueFamilyProperties(candidate, &count, families.data());

    auto fallback = -1;

    for (auto index = 0; index < families.size(); ++index)
    {
        const auto flags = families[index].queueFlags;

        if ((flags & VK_QUEUE_GRAPHICS_BIT) != 0
            && (flags & VK_QUEUE_COMPUTE_BIT) != 0)
            return index;

        if (fallback < 0 && (flags & VK_QUEUE_COMPUTE_BIT) != 0)
            fallback = index;
    }

    return fallback;
}

bool familyWritesTimestamps(VkPhysicalDevice candidate, std::uint32_t family)
{
    auto count = std::uint32_t {0};
    vkGetPhysicalDeviceQueueFamilyProperties(candidate, &count, nullptr);

    auto families = Vector<VkQueueFamilyProperties> {};
    families.resize(static_cast<int>(count));
    vkGetPhysicalDeviceQueueFamilyProperties(candidate, &count, families.data());

    if (static_cast<int>(family) >= families.size())
        return false;

    return families[static_cast<int>(family)].timestampValidBits > 0;
}

void addLayoutBinding(Vector<VkDescriptorSetLayoutBinding>& bindings,
                      int binding,
                      VkDescriptorType type,
                      VkShaderStageFlags stages)
{
    VkDescriptorSetLayoutBinding entry = {};
    entry.binding = static_cast<std::uint32_t>(binding);
    entry.descriptorType = type;
    entry.descriptorCount = 1;
    entry.stageFlags = stages;

    bindings.add(entry);
}

// Partially bound, so a shader may leave declared slots unwritten.
bool makePipelineLayouts(VkDevice device,
                         const Vector<VkDescriptorSetLayoutBinding>& bindings,
                         PipelineLayouts& layouts)
{
    auto flags = Vector<VkDescriptorBindingFlags> {};
    flags.resize(bindings.size(), VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT);

    VkDescriptorSetLayoutBindingFlagsCreateInfo bindingFlags = {};
    bindingFlags.sType =
        VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO;
    bindingFlags.bindingCount = static_cast<std::uint32_t>(flags.size());
    bindingFlags.pBindingFlags = flags.data();

    VkDescriptorSetLayoutCreateInfo layoutInfo = {};
    layoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    layoutInfo.pNext = &bindingFlags;
    layoutInfo.bindingCount = static_cast<std::uint32_t>(bindings.size());
    layoutInfo.pBindings = bindings.data();

    if (vkCreateDescriptorSetLayout(device, &layoutInfo, nullptr, &layouts.setLayout)
        != VK_SUCCESS)
        return false;

    VkPipelineLayoutCreateInfo pipelineInfo = {};
    pipelineInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    pipelineInfo.setLayoutCount = 1;
    pipelineInfo.pSetLayouts = &layouts.setLayout;

    if (vkCreatePipelineLayout(
            device, &pipelineInfo, nullptr, &layouts.pipelineLayout)
        == VK_SUCCESS)
        return true;

    vkDestroyDescriptorSetLayout(device, layouts.setLayout, nullptr);
    layouts.setLayout = VK_NULL_HANDLE;

    return false;
}

// Decoded from the index, which must agree with samplingIndex's packing.
VkSampler makeSampler(VkDevice device, int index)
{
    const auto linear = (index & 2) != 0;
    const auto repeat = (index & 1) != 0;

    const auto filter = linear ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
    const auto address = repeat ? VK_SAMPLER_ADDRESS_MODE_REPEAT
                                : VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;

    VkSamplerCreateInfo info = {};
    info.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    info.magFilter = filter;
    info.minFilter = filter;
    info.mipmapMode =
        linear ? VK_SAMPLER_MIPMAP_MODE_LINEAR : VK_SAMPLER_MIPMAP_MODE_NEAREST;
    info.addressModeU = address;
    info.addressModeV = address;
    info.addressModeW = address;
    info.maxLod = VK_LOD_CLAMP_NONE;
    info.borderColor = VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK;

    auto sampler = VkSampler {VK_NULL_HANDLE};

    if (vkCreateSampler(device, &info, nullptr, &sampler) != VK_SUCCESS)
        return VK_NULL_HANDLE;

    return sampler;
}
} // namespace

VulkanTextureBindings spirvTextureBindings(const Vector<std::uint32_t>& words,
                                           int firstBinding)
{
    auto bindings = VulkanTextureBindings {};

    // Magic number, version, generator, id bound, schema.
    constexpr auto headerWords = 5;

    if (words.size() <= headerWords)
        return bindings;

    constexpr auto opTypeImage = std::uint32_t {25};
    constexpr auto opTypeSampledImage = std::uint32_t {27};
    constexpr auto opTypePointer = std::uint32_t {32};
    constexpr auto opVariable = std::uint32_t {59};
    constexpr auto opDecorate = std::uint32_t {71};
    constexpr auto decorationBinding = std::uint32_t {33};

    // OpTypeImage's Sampled operand: 1 is read through a sampler, 2 read or
    // written with the image instructions.
    constexpr auto sampledThroughASampler = std::uint32_t {1};

    const auto bound = static_cast<int>(words[3]);

    if (bound <= 0)
        return bindings;

    enum class IdKind
    {
        unknown,
        sampledImage, // OpTypeSampledImage: a combined image sampler
        storageImage, // OpTypeImage with Sampled = 2
        readImage // OpTypeImage with Sampled = 1, unpaired
    };

    auto kinds = Vector<IdKind> {};
    kinds.resize(bound, IdKind::unknown);

    // For an OpTypePointer, the id of what it points at; 0 for everything else.
    auto pointee = Vector<std::uint32_t> {};
    pointee.resize(bound, 0u);

    // Collected rather than resolved inline: a module puts its annotations ahead
    // of its types.
    auto variableType = Vector<std::uint32_t> {};
    variableType.resize(bound, 0u);

    auto slotOfId = Vector<int> {};
    slotOfId.resize(bound, -1);

    const auto inRange = [&](std::uint32_t id)
    { return id < (std::uint32_t) bound; };

    auto index = headerWords;

    while (index < words.size())
    {
        const auto instruction = words[index];
        const auto wordCount = static_cast<int>(instruction >> 16);
        const auto opcode = instruction & 0xffffu;

        // A zero-length instruction cannot be stepped over, and one past the end
        // means the module is not what it says it is.
        if (wordCount <= 0 || index + wordCount > words.size())
            break;

        if (opcode == opDecorate && wordCount >= 4
            && words[index + 2] == decorationBinding && inRange(words[index + 1]))
        {
            const auto slot = static_cast<int>(words[index + 3]) - firstBinding;

            if (slot >= 0 && slot < maxTextureSlots)
                slotOfId[static_cast<int>(words[index + 1])] = slot;
        }
        else if (opcode == opTypeImage && wordCount >= 9
                 && inRange(words[index + 1]))
        {
            kinds[static_cast<int>(words[index + 1])] =
                words[index + 7] == sampledThroughASampler ? IdKind::readImage
                                                           : IdKind::storageImage;
        }
        else if (opcode == opTypeSampledImage && wordCount >= 3
                 && inRange(words[index + 1]))
        {
            kinds[static_cast<int>(words[index + 1])] = IdKind::sampledImage;
        }
        else if (opcode == opTypePointer && wordCount >= 4
                 && inRange(words[index + 1]))
        {
            pointee[static_cast<int>(words[index + 1])] = words[index + 3];
        }
        else if (opcode == opVariable && wordCount >= 4 && inRange(words[index + 2]))
        {
            variableType[static_cast<int>(words[index + 2])] = words[index + 1];
        }

        index += wordCount;
    }

    for (auto id = 0; id < bound; ++id)
    {
        const auto slot = slotOfId[id];

        if (slot < 0 || variableType[id] == 0u || !inRange(variableType[id]))
            continue;

        const auto pointed = pointee[static_cast<int>(variableType[id])];

        if (!inRange(pointed))
            continue;

        switch (kinds[static_cast<int>(pointed)])
        {
            case IdKind::sampledImage:
                bindings.add(slot, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER);
                break;

            case IdKind::storageImage:
                bindings.add(slot, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE);
                break;

            case IdKind::readImage:
                bindings.add(slot, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE);
                break;

            case IdKind::unknown:
                break;
        }
    }

    return bindings;
}

VulkanShared::VulkanShared()
{
    createAll();
}

VulkanShared::~VulkanShared()
{
    if (allocator != nullptr)
        vmaDestroyAllocator(allocator);

    if (device != VK_NULL_HANDLE)
    {
        savePipelineCache();

        if (pipelineCache != VK_NULL_HANDLE)
            vkDestroyPipelineCache(device, pipelineCache, nullptr);

        for (auto& sampler: samplers)
            if (sampler != VK_NULL_HANDLE)
                vkDestroySampler(device, sampler, nullptr);

        for (const auto& layouts: {computeLayouts, renderLayouts})
        {
            if (layouts.pipelineLayout != VK_NULL_HANDLE)
                vkDestroyPipelineLayout(device, layouts.pipelineLayout, nullptr);

            if (layouts.setLayout != VK_NULL_HANDLE)
                vkDestroyDescriptorSetLayout(device, layouts.setLayout, nullptr);
        }

        vkDestroyDevice(device, nullptr);
    }

    if (instance != VK_NULL_HANDLE)
    {
        if (messenger != VK_NULL_HANDLE)
            vkDestroyDebugUtilsMessengerEXT(instance, messenger, nullptr);

        vkDestroyInstance(instance, nullptr);
    }
}

void VulkanShared::createAll()
{
    // volkInitialize dlopens libvulkan.so.1.
    if (volkInitialize() != VK_SUCCESS)
        return;

    if (!createInstance())
        return;

    createDebugMessenger();

    if (!selectPhysicalDevice() || !createDevice() || !createAllocator()
        || !createComputeLayouts() || !createRenderLayouts())
    {
        return;
    }

    createPipelineCache();

    for (auto index = 0; index < samplingConfigurations; ++index)
    {
        samplers[index] = makeSampler(device, index);

        if (samplers[index] == VK_NULL_HANDLE)
            LOG("Vulkan: sampler ", index, " could not be created");
    }

    // The 90 ms glslang spends on its symbol tables, kept out of the first frame.
    Spirv::warmUp();
}

bool VulkanShared::createInstance()
{
    // The loader's version caps what apiVersion may ask for. A 1.0 loader has no
    // vkEnumerateInstanceVersion at all, which under volk is a null pointer.
    auto loaderVersion = std::uint32_t {0};

    if (vkEnumerateInstanceVersion == nullptr
        || vkEnumerateInstanceVersion(&loaderVersion) != VK_SUCCESS
        || loaderVersion < VK_API_VERSION_1_3)
    {
        LOG("Vulkan: the loader is below 1.3; no device will be created");
        return false;
    }

    VkApplicationInfo application = {};
    application.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    application.pApplicationName = "eacp";
    application.pEngineName = "eacp";
    application.apiVersion = VK_API_VERSION_1_3;

    auto layers = Vector<const char*> {};
    auto extensions = Vector<const char*> {};

    if (wantsValidation())
    {
        if (hasInstanceLayer("VK_LAYER_KHRONOS_validation"))
            layers.add("VK_LAYER_KHRONOS_validation");
        else
            LOG("Vulkan: EACP_VK_VALIDATION is set but the layer is not "
                "installed");

        if (hasInstanceExtension(VK_EXT_DEBUG_UTILS_EXTENSION_NAME))
            extensions.add(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
    }

    // Asked for rather than required: a headless ICD offers neither.
    const auto surfaceOffered =
        hasInstanceExtension(VK_KHR_SURFACE_EXTENSION_NAME)
        && hasInstanceExtension(VK_KHR_WAYLAND_SURFACE_EXTENSION_NAME);

    if (surfaceOffered)
    {
        extensions.add(VK_KHR_SURFACE_EXTENSION_NAME);
        extensions.add(VK_KHR_WAYLAND_SURFACE_EXTENSION_NAME);
    }

    VkInstanceCreateInfo info = {};
    info.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    info.pApplicationInfo = &application;
    info.enabledLayerCount = static_cast<std::uint32_t>(layers.size());
    info.ppEnabledLayerNames = layers.data();
    info.enabledExtensionCount = static_cast<std::uint32_t>(extensions.size());
    info.ppEnabledExtensionNames = extensions.data();

    if (vkCreateInstance(&info, nullptr, &instance) != VK_SUCCESS)
        return false;

    surfaceExtensionsEnabled = surfaceOffered;

    volkLoadInstanceOnly(instance);
    return true;
}

void VulkanShared::createDebugMessenger()
{
    if (!wantsValidation() || vkCreateDebugUtilsMessengerEXT == nullptr)
        return;

    VkDebugUtilsMessengerCreateInfoEXT info = {};
    info.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT;
    info.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT
                           | VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
    info.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT
                       | VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT
                       | VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
    info.pfnUserCallback = vulkanDebugCallback;

    vkCreateDebugUtilsMessengerEXT(instance, &info, nullptr, &messenger);
}

bool VulkanShared::selectPhysicalDevice()
{
    auto count = std::uint32_t {0};
    vkEnumeratePhysicalDevices(instance, &count, nullptr);

    if (count == 0)
        return false;

    auto candidates = Vector<VkPhysicalDevice> {};
    candidates.resize(static_cast<int>(count));
    vkEnumeratePhysicalDevices(instance, &count, candidates.data());

    const auto preferSoftware = prefersSoftwareDevice();

    auto best = VkPhysicalDevice {VK_NULL_HANDLE};
    auto bestRank = -1;
    auto sawIncompleteDevice = false;

    for (auto candidate: candidates)
    {
        VkPhysicalDeviceProperties candidateProperties = {};
        vkGetPhysicalDeviceProperties(candidate, &candidateProperties);

        if (candidateProperties.apiVersion < VK_API_VERSION_1_3)
            continue;

        if (!probeFeatures(candidate).allPresent())
        {
            sawIncompleteDevice = true;
            continue;
        }

        if (findQueueFamily(candidate) < 0)
            continue;

        const auto isSoftware =
            candidateProperties.deviceType == VK_PHYSICAL_DEVICE_TYPE_CPU;

        // Inverts the order rather than filtering, so a real GPU is still found.
        const auto rank =
            preferSoftware
                ? (isSoftware ? 5 : deviceRank(candidateProperties.deviceType))
                : deviceRank(candidateProperties.deviceType);

        if (rank > bestRank)
        {
            bestRank = rank;
            best = candidate;
            properties = candidateProperties;
        }
    }

    if (best == VK_NULL_HANDLE)
    {
        if (sawIncompleteDevice)
            LOG("Vulkan: no device offers the 1.3 feature set eacp needs "
                "(timeline semaphores, synchronization2, dynamic rendering, "
                "partially bound descriptors, format-less storage image writes)");

        return false;
    }

    physicalDevice = best;
    queueFamily = static_cast<std::uint32_t>(findQueueFamily(physicalDevice));
    adapterName = properties.deviceName;

    vkGetPhysicalDeviceFeatures(physicalDevice, &features);

    timestampsSupported = properties.limits.timestampComputeAndGraphics == VK_TRUE
                          && familyWritesTimestamps(physicalDevice, queueFamily);

    depthResolvesBySampleZero = queryDepthResolvesBySampleZero(physicalDevice);

    return true;
}

bool VulkanShared::createDevice()
{
    const auto priority = 1.0f;

    VkDeviceQueueCreateInfo queueInfo = {};
    queueInfo.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    queueInfo.queueFamilyIndex = queueFamily;
    queueInfo.queueCount = 1;
    queueInfo.pQueuePriorities = &priority;

    // Exactly the five probeFeatures asked about.
    VkPhysicalDeviceVulkan13Features features13 = {};
    features13.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES;
    features13.synchronization2 = VK_TRUE;
    features13.dynamicRendering = VK_TRUE;

    VkPhysicalDeviceVulkan12Features features12 = {};
    features12.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES;
    features12.pNext = &features13;
    features12.timelineSemaphore = VK_TRUE;
    features12.descriptorBindingPartiallyBound = VK_TRUE;

    VkPhysicalDeviceFeatures2 enabled = {};
    enabled.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
    enabled.pNext = &features12;
    enabled.features.shaderStorageImageWriteWithoutFormat = VK_TRUE;

    // Absent rather than fatal; GPUView then stays on the off-screen path.
    auto extensions = Vector<const char*> {};

    const auto swapchainOffered =
        surfaceExtensionsEnabled
        && hasDeviceExtension(physicalDevice, VK_KHR_SWAPCHAIN_EXTENSION_NAME);

    if (swapchainOffered)
        extensions.add(VK_KHR_SWAPCHAIN_EXTENSION_NAME);

    VkDeviceCreateInfo info = {};
    info.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    info.pNext = &enabled;
    info.queueCreateInfoCount = 1;
    info.pQueueCreateInfos = &queueInfo;
    info.enabledExtensionCount = static_cast<std::uint32_t>(extensions.size());
    info.ppEnabledExtensionNames = extensions.data();

    if (vkCreateDevice(physicalDevice, &info, nullptr, &device) != VK_SUCCESS)
    {
        device = VK_NULL_HANDLE;
        return false;
    }

    presentationSupported = swapchainOffered;

    // One device in the process, so the dispatch table can be volk's global one.
    volkLoadDevice(device);
    vkGetDeviceQueue(device, queueFamily, 0, &queue);

    return queue != VK_NULL_HANDLE;
}

bool VulkanShared::createAllocator()
{
    // No symbols to link against under VK_NO_PROTOTYPES.
    VmaVulkanFunctions functions = {};
    functions.vkGetInstanceProcAddr = vkGetInstanceProcAddr;
    functions.vkGetDeviceProcAddr = vkGetDeviceProcAddr;

    VmaAllocatorCreateInfo info = {};
    info.vulkanApiVersion = VK_API_VERSION_1_3;
    info.instance = instance;
    info.physicalDevice = physicalDevice;
    info.device = device;
    info.pVulkanFunctions = &functions;

    return vmaCreateAllocator(&info, &allocator) == VK_SUCCESS;
}

PipelineLayouts makeComputeLayouts(VkDevice device,
                                   const VulkanTextureBindings& textures)
{
    // Laid out exactly as Codegen/ShaderBindings.h prints it. Only the texture
    // slots the module declares get a binding, at the type declared.
    auto layouts = PipelineLayouts {};

    auto bindings = Vector<VkDescriptorSetLayoutBinding> {};

    const auto addBinding = [&](int binding, VkDescriptorType type)
    { addLayoutBinding(bindings, binding, type, VK_SHADER_STAGE_COMPUTE_BIT); };

    for (auto slot = 0; slot < maxBufferSlots; ++slot)
        addBinding(vulkanComputeBufferBinding(slot),
                   VK_DESCRIPTOR_TYPE_STORAGE_BUFFER);

    for (auto slot = 0; slot < maxTextureSlots; ++slot)
        if (textures.has(slot))
            addBinding(vulkanComputeTextureBinding(slot), textures.typeAt(slot));

    addBinding(vulkanComputeUniformBinding,
               VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC);

    if (!makePipelineLayouts(device, bindings, layouts))
        return {};

    return layouts;
}

bool VulkanShared::createRenderLayouts()
{
    // Laid out exactly as Codegen/ShaderBindings.h prints it for a render shader.
    // Not immutable samplers: the sampler travels in the descriptor write.
    auto bindings = Vector<VkDescriptorSetLayoutBinding> {};

    const auto addBinding = [&](int binding, VkDescriptorType type)
    {
        addLayoutBinding(bindings,
                         binding,
                         type,
                         VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT);
    };

    addBinding(vulkanUniformBinding, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC);

    for (auto slot = 0; slot < maxTextureSlots; ++slot)
        addBinding(vulkanTextureBinding(slot),
                   VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER);

    for (auto slot = 0; slot < maxBufferSlots; ++slot)
        addBinding(vulkanBufferBinding(slot), VK_DESCRIPTOR_TYPE_STORAGE_BUFFER);

    return makePipelineLayouts(device, bindings, renderLayouts);
}

bool VulkanShared::createComputeLayouts()
{
    // The layout every kernel that declares no texture binds through.
    computeLayouts = makeComputeLayouts(device, {});

    return computeLayouts.isValid();
}

void VulkanShared::createPipelineCache()
{
    const auto blob = readFileBytes(vulkanPipelineCachePath(properties));

    VkPipelineCacheCreateInfo info = {};
    info.sType = VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO;

    if (pipelineCacheHeaderMatches(blob, properties))
    {
        info.initialDataSize = static_cast<std::size_t>(blob.size());
        info.pInitialData = blob.data();
    }

    if (vkCreatePipelineCache(device, &info, nullptr, &pipelineCache) != VK_SUCCESS)
        pipelineCache = VK_NULL_HANDLE;
}

void VulkanShared::savePipelineCache() const
{
    if (pipelineCache == VK_NULL_HANDLE)
        return;

    auto bytes = std::size_t {0};

    if (vkGetPipelineCacheData(device, pipelineCache, &bytes, nullptr) != VK_SUCCESS
        || bytes == 0)
        return;

    auto blob = Vector<std::byte> {};
    blob.resize(static_cast<int>(bytes));

    if (vkGetPipelineCacheData(device, pipelineCache, &bytes, blob.data())
        != VK_SUCCESS)
        return;

    writeFileAtomically(vulkanPipelineCachePath(properties), blob);
}

VulkanShared& getVulkanShared()
{
    static auto shared = VulkanShared();
    return shared;
}

VulkanContext::VulkanContext()
    : owningThreadId(currentThreadId())
{
    createAll();
}

VulkanContext::~VulkanContext()
{
    if (isValid())
        waitIdle();

    releaseAll();
}

void VulkanContext::createAll()
{
    auto& shared = getVulkanShared();

    if (!shared.isValid())
        return;

    VkSemaphoreTypeCreateInfo type = {};
    type.sType = VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO;
    type.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE;
    type.initialValue = 0;

    VkSemaphoreCreateInfo info = {};
    info.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
    info.pNext = &type;

    if (vkCreateSemaphore(shared.getDevice(), &info, nullptr, &timeline)
        != VK_SUCCESS)
        timeline = VK_NULL_HANDLE;
}

void VulkanContext::releaseAll()
{
    auto vulkanDevice = getVulkanShared().getDevice();

    if (vulkanDevice == VK_NULL_HANDLE)
        return;

    // waitIdle has run, so nothing the GPU still reads is among these.
    for (auto& entry: retired)
        entry.destroy();

    retired.clear();

    for (auto& page: constantPages)
        vmaDestroyBuffer(getAllocator(), page.buffer, page.allocation);

    constantPages.clear();

    destroyPool(staging);
    destroyPool(readback);

    for (auto& commands: pool)
    {
        for (auto& chunk: commands->uploads)
            vmaDestroyBuffer(getAllocator(), chunk.buffer, chunk.allocation);

        for (auto descriptorPool: commands->descriptorPools)
            vkDestroyDescriptorPool(vulkanDevice, descriptorPool, nullptr);

        if (commands->pool != VK_NULL_HANDLE)
            vkDestroyCommandPool(vulkanDevice, commands->pool, nullptr);
    }

    openRecording = nullptr;
    pool.clear();
    available.clear();

    if (timeline != VK_NULL_HANDLE)
        vkDestroySemaphore(vulkanDevice, timeline, nullptr);

    timeline = VK_NULL_HANDLE;
}

void VulkanContext::destroyPool(Vector<PooledBuffer>& buffers)
{
    for (auto& slot: buffers)
        vmaDestroyBuffer(getAllocator(), slot.buffer, slot.allocation);

    buffers.clear();
}

void VulkanContext::assertOwningThread() const
{
    const auto onOwningThread = mainThreadOwned
                                    ? Threads::isMainThread()
                                    : currentThreadId() == owningThreadId;

    assert(onOwningThread
           && "eacp: a GPU::Device belongs to the thread that made it - give "
              "each thread its own");

    (void) onOwningThread;
}

CommandContext* VulkanContext::acquire()
{
    assertOwningThread();

    if (!isValid())
        return nullptr;

    purgeRetired();

    auto vulkanDevice = getDevice();

    auto recycled =
        std::find_if(available.begin(),
                     available.end(),
                     [this](CommandContext* candidate)
                     { return hasCompleted(candidate->completionValue); });

    CommandContext* commands = nullptr;

    if (recycled != available.end())
    {
        commands = *recycled;
        available.erase(recycled);

        // Resetting the pool, not the buffer, returns the command memory.
        vkResetCommandPool(vulkanDevice, commands->pool, 0);
        commands->rewindUploads();

        for (auto descriptorPool: commands->descriptorPools)
            vkResetDescriptorPool(vulkanDevice, descriptorPool, 0);

        commands->descriptorCursor = 0;
    }
    else
    {
        auto fresh = makeOwned<CommandContext>();

        VkCommandPoolCreateInfo poolInfo = {};
        poolInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
        poolInfo.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
        poolInfo.queueFamilyIndex = getVulkanShared().getQueueFamily();

        if (vkCreateCommandPool(vulkanDevice, &poolInfo, nullptr, &fresh->pool)
            != VK_SUCCESS)
            return nullptr;

        VkCommandBufferAllocateInfo bufferInfo = {};
        bufferInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        bufferInfo.commandPool = fresh->pool;
        bufferInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        bufferInfo.commandBufferCount = 1;

        if (vkAllocateCommandBuffers(vulkanDevice, &bufferInfo, &fresh->buffer)
            != VK_SUCCESS)
        {
            vkDestroyCommandPool(vulkanDevice, fresh->pool, nullptr);
            return nullptr;
        }

        commands = fresh.get();
        pool.add(std::move(fresh));
    }

    VkCommandBufferBeginInfo begin = {};
    begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;

    if (vkBeginCommandBuffer(commands->buffer, &begin) != VK_SUCCESS)
    {
        available.push_back(commands);
        return nullptr;
    }

    commands->context = this;
    commands->completionValue = 0;
    commands->recordingId = ++recordingCounter;
    return commands;
}

std::uint64_t VulkanContext::submit(CommandContext* commands, const SubmitSync& sync)
{
    assertOwningThread();

    if (commands == nullptr || !isValid())
        return 0;

    // One global barrier ends every recording, which is what makes the
    // per-recording tracking in transitionForUse correct.
    VkMemoryBarrier2 barrier = {};
    barrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2;
    barrier.srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    barrier.srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT;
    barrier.dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    barrier.dstAccessMask =
        VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT;

    VkDependencyInfo dependency = {};
    dependency.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
    dependency.memoryBarrierCount = 1;
    dependency.pMemoryBarriers = &barrier;

    vkCmdPipelineBarrier2(commands->buffer, &dependency);

    if (vkEndCommandBuffer(commands->buffer) != VK_SUCCESS)
    {
        // Nothing reached the GPU, so the slots are free at once.
        reportFailedRecording();
        returnStaging(*commands, 0);
        returnConstantPages(*commands, 0);
        available.push_back(commands);
        return 0;
    }

    const auto value = nextValue++;

    VkCommandBufferSubmitInfo bufferInfo = {};
    bufferInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO;
    bufferInfo.commandBuffer = commands->buffer;

    VkSemaphoreSubmitInfo signals[2] = {};
    signals[0].sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO;
    signals[0].semaphore = timeline;
    signals[0].value = value;
    signals[0].stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;

    auto signalCount = std::uint32_t {1};

    if (sync.signal != VK_NULL_HANDLE)
    {
        signals[signalCount].sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO;
        signals[signalCount].semaphore = sync.signal;
        signals[signalCount].stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
        ++signalCount;
    }

    // Waited on at COLOR_ATTACHMENT_OUTPUT, which imageAcquired also names as its
    // source stage, so the first transition cannot overtake the acquire.
    VkSemaphoreSubmitInfo wait = {};
    wait.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO;
    wait.semaphore = sync.wait;
    wait.stageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;

    VkSubmitInfo2 submitInfo = {};
    submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2;
    submitInfo.commandBufferInfoCount = 1;
    submitInfo.pCommandBufferInfos = &bufferInfo;
    submitInfo.signalSemaphoreInfoCount = signalCount;
    submitInfo.pSignalSemaphoreInfos = signals;

    if (sync.wait != VK_NULL_HANDLE)
    {
        submitInfo.waitSemaphoreInfoCount = 1;
        submitInfo.pWaitSemaphoreInfos = &wait;
    }

    {
        auto lock = std::lock_guard<std::mutex> {getVulkanShared().getQueueMutex()};

        if (vkQueueSubmit2(getQueue(), 1, &submitInfo, VK_NULL_HANDLE) != VK_SUCCESS)
        {
            returnStaging(*commands, 0);
            returnConstantPages(*commands, 0);
            available.push_back(commands);
            return 0;
        }
    }

    commands->completionValue = value;
    lastSubmittedValue = value;
    returnStaging(*commands, value);
    returnConstantPages(*commands, value);
    available.push_back(commands);
    return value;
}

void VulkanContext::reportFailedRecording() const
{
    static auto reported = false;

    if (reported)
        return;

    reported = true;
    LOG("VulkanContext: a recording failed to close and was not submitted");
}

void VulkanContext::discard(CommandContext* commands)
{
    assertOwningThread();

    if (commands == nullptr)
        return;

    vkEndCommandBuffer(commands->buffer);
    returnStaging(*commands, 0);
    returnConstantPages(*commands, 0);
    commands->completionValue = 0;
    available.push_back(commands);
}

bool VulkanContext::hasCompleted(std::uint64_t value) const
{
    if (timeline == VK_NULL_HANDLE)
        return true;

    auto current = std::uint64_t {0};

    // A counter that cannot be read is a lost device. Answering "complete" is
    // deliberate: nothing queued will finish, and every waitFor would spin.
    if (vkGetSemaphoreCounterValue(getVulkanShared().getDevice(), timeline, &current)
        != VK_SUCCESS)
        return true;

    return current >= value;
}

void VulkanContext::waitFor(std::uint64_t value)
{
    if (timeline == VK_NULL_HANDLE || value == 0 || hasCompleted(value))
        return;

    VkSemaphoreWaitInfo info = {};
    info.sType = VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO;
    info.semaphoreCount = 1;
    info.pSemaphores = &timeline;
    info.pValues = &value;

    vkWaitSemaphores(getVulkanShared().getDevice(), &info, UINT64_MAX);
}

// The queue being FIFO, the newest value covers everything submitted.
void VulkanContext::waitIdle()
{
    waitFor(lastSubmittedValue);
}

void VulkanContext::notifyWhenCompleted(std::uint64_t value, Callback done)
{
    if (hasCompleted(value))
    {
        done();
        return;
    }

    pendingCompletions.add({value, std::move(done)});

    if (!completionPoll.has_value())
        completionPoll.emplace([this] { pollCompletions(); }, completionPollHz);
}

void VulkanContext::pollCompletions()
{
    // The callbacks fire after the list is rebuilt: one may commit more work,
    // appending to it.
    auto ready = Vector<Callback> {};
    auto stillPending = Vector<PendingCompletion> {};

    for (auto& pending: pendingCompletions)
    {
        if (hasCompleted(pending.completionValue))
            ready.add(std::move(pending.done));
        else
            stillPending.add(std::move(pending));
    }

    pendingCompletions = std::move(stillPending);

    if (pendingCompletions.empty())
        completionPoll.reset();

    for (auto& done: ready)
        done();
}

void VulkanContext::deferRelease(Callback destroy)
{
    if (destroy == nullptr)
        return;

    // Unstamped: a recording naming the object may still be open. Stamping with
    // the next submit's value is wrong - an upload's own recording signals first.
    retired.add({std::move(destroy), 0, false});
}

void VulkanContext::deferReleaseBuffer(VkBuffer buffer, VmaAllocation allocation)
{
    if (buffer == VK_NULL_HANDLE)
        return;

    deferRelease([allocator = getAllocator(), buffer, allocation]
                 { vmaDestroyBuffer(allocator, buffer, allocation); });
}

void VulkanContext::purgeRetired()
{
    // Nothing is recording, so everything retired has been submitted and
    // lastSubmittedValue is at or past all of it - the first sound moment.
    if (available.size() == pool.size())
    {
        for (auto& entry: retired)
        {
            if (!entry.stamped)
            {
                entry.completionValue = lastSubmittedValue;
                entry.stamped = true;
            }
        }
    }

    // Each goes as its own value passes; waiting for the timeline to pass
    // everything frees nothing under continuous rendering.
    retired.eraseIf(
        [this](Retired& entry)
        {
            if (!entry.stamped || !hasCompleted(entry.completionValue))
                return false;

            entry.destroy();
            return true;
        });
}

bool VulkanContext::makeHostBuffer(std::size_t bytes,
                                   VkBufferUsageFlags usage,
                                   bool readBack,
                                   VkBuffer& buffer,
                                   VmaAllocation& allocation,
                                   std::byte*& mapped)
{
    if (bytes == 0)
        return false;

    VkBufferCreateInfo info = {};
    info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    info.size = bytes;
    info.usage = usage;
    info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

    // Coherent, so no call site flushes. Cached on the readback side,
    // write-combined memory being far slower to read.
    VmaAllocationCreateInfo allocationInfo = {};
    allocationInfo.usage = VMA_MEMORY_USAGE_AUTO;
    allocationInfo.flags =
        VMA_ALLOCATION_CREATE_MAPPED_BIT
        | (readBack ? VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT
                    : VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT);
    allocationInfo.requiredFlags =
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;

    if (readBack)
        allocationInfo.preferredFlags = VK_MEMORY_PROPERTY_HOST_CACHED_BIT;

    VmaAllocationInfo result = {};

    if (vmaCreateBuffer(
            getAllocator(), &info, &allocationInfo, &buffer, &allocation, &result)
        != VK_SUCCESS)
        return false;

    mapped = static_cast<std::byte*>(result.pMappedData);

    if (mapped == nullptr)
    {
        vmaDestroyBuffer(getAllocator(), buffer, allocation);
        buffer = VK_NULL_HANDLE;
        allocation = nullptr;
        return false;
    }

    return true;
}

CommandContext::UploadChunk* VulkanContext::uploadRoomFor(CommandContext& commands,
                                                          std::size_t bytes)
{
    // Forward only: a chunk the cursor passed was too full for an earlier
    // request.
    while (commands.uploadCursor < commands.uploads.size())
    {
        auto& chunk = commands.uploads[commands.uploadCursor];

        if (chunk.capacity - chunk.used >= bytes)
            return &chunk;

        ++commands.uploadCursor;
    }

    auto chunk = CommandContext::UploadChunk {};
    chunk.capacity = std::max(vulkanUploadChunkBytes, bytes);

    if (!makeHostBuffer(chunk.capacity,
                        VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                        false,
                        chunk.buffer,
                        chunk.allocation,
                        chunk.mapped))
        return nullptr;

    return &commands.uploads.add(std::move(chunk));
}

UploadRange VulkanContext::allocateUpload(CommandContext& commands,
                                          std::size_t bytes)
{
    if (bytes == 0)
        return {};

    const auto alignment = std::max<std::size_t>(
        static_cast<std::size_t>(getVulkanShared()
                                     .getProperties()
                                     .limits.optimalBufferCopyOffsetAlignment),
        16);

    const auto aligned = roundUpTo(bytes, alignment);
    auto* chunk = uploadRoomFor(commands, aligned);

    if (chunk == nullptr)
        return {};

    auto range = UploadRange {};
    range.buffer = chunk->buffer;
    range.mapped = chunk->mapped + chunk->used;
    range.offset = static_cast<VkDeviceSize>(chunk->used);

    chunk->used += aligned;

    return range;
}

VulkanContext::ConstantPage* VulkanContext::pageFor(CommandContext& commands,
                                                    std::size_t bytes)
{
    if (!commands.constantsTaken.empty())
    {
        const auto lastTaken =
            commands.constantsTaken[commands.constantsTaken.getLastElementIndex()];
        auto& open = constantPages[lastTaken];

        if (open.remaining() >= bytes)
            return &open;
    }

    const auto take = [&](int index) -> ConstantPage*
    {
        auto& page = constantPages[index];
        page.lent = true;
        page.used = 0;
        commands.constantsTaken.add(index);
        return &page;
    };

    for (auto index = 0; index < constantPages.size(); ++index)
    {
        const auto& page = constantPages[index];

        if (!page.lent && hasCompleted(page.freeAt) && page.bytes >= bytes)
            return take(index);
    }

    auto page = ConstantPage {};
    page.bytes = std::max(vulkanConstantPageBytes, bytes);

    if (!makeHostBuffer(page.bytes,
                        VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
                        false,
                        page.buffer,
                        page.allocation,
                        page.mapped))
        return nullptr;

    constantPages.add(std::move(page));
    return take(constantPages.size() - 1);
}

ConstantRange VulkanContext::uploadConstants(CommandContext& commands,
                                             const void* data,
                                             std::size_t bytes)
{
    if (data == nullptr || bytes == 0)
        return {};

    // The range is the shader's block; the step is where the next may start.
    const auto range = roundUpTo(bytes, vulkanUniformBlockRounding);
    const auto alignment = std::max<std::size_t>(
        static_cast<std::size_t>(getVulkanShared()
                                     .getProperties()
                                     .limits.minUniformBufferOffsetAlignment),
        vulkanUniformBlockRounding);
    const auto step = roundUpTo(range, alignment);

    auto* page = pageFor(commands, step);

    if (page == nullptr)
        return {};

    const auto offset = page->used;
    page->used += step;

    std::memcpy(page->mapped + offset, data, bytes);

    return {page->buffer,
            static_cast<VkDeviceSize>(offset),
            static_cast<VkDeviceSize>(range)};
}

VkBuffer VulkanContext::acquirePooled(Vector<PooledBuffer>& buffers,
                                      Vector<int>& taken,
                                      std::size_t bytes,
                                      VkBufferUsageFlags usage,
                                      bool readBack,
                                      std::byte*& mapped)
{
    if (!isValid() || bytes == 0)
        return VK_NULL_HANDLE;

    const auto isFree = [this](const PooledBuffer& slot)
    { return !slot.lent && hasCompleted(slot.freeAt); };

    for (auto index = 0; index < buffers.size(); ++index)
    {
        auto& slot = buffers[index];

        if (isFree(slot) && slot.bytes >= bytes)
        {
            slot.lent = true;
            taken.add(index);
            mapped = slot.mapped;
            return slot.buffer;
        }
    }

    for (auto index = 0; index < buffers.size(); ++index)
    {
        auto& slot = buffers[index];

        if (!isFree(slot))
            continue;

        auto grown = VkBuffer {VK_NULL_HANDLE};
        auto grownAllocation = VmaAllocation {nullptr};
        std::byte* grownMapped = nullptr;

        if (!makeHostBuffer(
                bytes, usage, readBack, grown, grownAllocation, grownMapped))
            return VK_NULL_HANDLE;

        deferReleaseBuffer(slot.buffer, slot.allocation);

        slot.buffer = grown;
        slot.allocation = grownAllocation;
        slot.mapped = grownMapped;
        slot.bytes = bytes;
        slot.lent = true;
        taken.add(index);
        mapped = grownMapped;
        return slot.buffer;
    }

    auto slot = PooledBuffer {};

    if (!makeHostBuffer(
            bytes, usage, readBack, slot.buffer, slot.allocation, slot.mapped))
        return VK_NULL_HANDLE;

    slot.bytes = bytes;
    slot.lent = true;

    auto buffer = slot.buffer;
    mapped = slot.mapped;
    buffers.add(std::move(slot));
    taken.add(buffers.size() - 1);
    return buffer;
}

VkBuffer VulkanContext::acquireStagingBuffer(CommandContext& commands,
                                             std::size_t bytes,
                                             std::byte*& mapped)
{
    return acquirePooled(staging,
                         commands.stagingTaken,
                         bytes,
                         VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                         false,
                         mapped);
}

VkBuffer VulkanContext::acquireReadbackBuffer(CommandContext& commands,
                                              std::size_t bytes,
                                              std::byte*& mapped)
{
    return acquirePooled(readback,
                         commands.readbackTaken,
                         bytes,
                         VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                         true,
                         mapped);
}

void VulkanContext::returnPooled(Vector<PooledBuffer>& buffers,
                                 Vector<int>& taken,
                                 std::uint64_t freeAt)
{
    for (auto index: taken)
    {
        if (index < 0 || index >= buffers.size())
            continue;

        buffers[index].lent = false;
        buffers[index].freeAt = freeAt;
    }

    taken.clear();
}

void VulkanContext::returnStaging(CommandContext& commands, std::uint64_t freeAt)
{
    returnPooled(staging, commands.stagingTaken, freeAt);
    returnPooled(readback, commands.readbackTaken, freeAt);
}

void VulkanContext::returnConstantPages(CommandContext& commands,
                                        std::uint64_t freeAt)
{
    for (auto index: commands.constantsTaken)
    {
        if (index < 0 || index >= constantPages.size())
            continue;

        constantPages[index].lent = false;
        constantPages[index].freeAt = freeAt;
    }

    commands.constantsTaken.clear();
}

VkDescriptorSet VulkanContext::allocateDescriptorSet(CommandContext& commands,
                                                     VkDescriptorSetLayout layout)
{
    if (layout == VK_NULL_HANDLE || !isValid())
        return VK_NULL_HANDLE;

    auto vulkanDevice = getDevice();

    const auto allocateFrom = [&](VkDescriptorPool from) -> VkDescriptorSet
    {
        VkDescriptorSetAllocateInfo info = {};
        info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        info.descriptorPool = from;
        info.descriptorSetCount = 1;
        info.pSetLayouts = &layout;

        auto set = VkDescriptorSet {VK_NULL_HANDLE};

        if (vkAllocateDescriptorSets(vulkanDevice, &info, &set) != VK_SUCCESS)
            return VK_NULL_HANDLE;

        return set;
    };

    while (commands.descriptorCursor < commands.descriptorPools.size())
    {
        if (auto set =
                allocateFrom(commands.descriptorPools[commands.descriptorCursor]))
            return set;

        ++commands.descriptorCursor;
    }

    // Both image types at full width, which of the two a slot takes being per
    // shader. The overcount is headroom, not memory.
    const VkDescriptorPoolSize sizes[] = {
        {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
         vulkanSetsPerDescriptorPool * static_cast<std::uint32_t>(maxBufferSlots)},
        {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
         vulkanSetsPerDescriptorPool * static_cast<std::uint32_t>(maxTextureSlots)},
        {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
         vulkanSetsPerDescriptorPool * static_cast<std::uint32_t>(maxTextureSlots)},
        {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, vulkanSetsPerDescriptorPool}};

    VkDescriptorPoolCreateInfo info = {};
    info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    info.maxSets = vulkanSetsPerDescriptorPool;
    info.poolSizeCount = static_cast<std::uint32_t>(std::size(sizes));
    info.pPoolSizes = sizes;

    auto fresh = VkDescriptorPool {VK_NULL_HANDLE};

    if (vkCreateDescriptorPool(vulkanDevice, &info, nullptr, &fresh) != VK_SUCCESS)
        return VK_NULL_HANDLE;

    commands.descriptorPools.add(fresh);
    commands.descriptorCursor = commands.descriptorPools.size() - 1;

    return allocateFrom(fresh);
}
} // namespace eacp::GPU
