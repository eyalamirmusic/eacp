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
#include <functional>
#include <thread>

namespace eacp::GPU
{
namespace
{
// One page of the constant ring, sized so a recording of a few hundred
// dispatches fits in a single page and never allocates mid-frame.
constexpr std::size_t vulkanConstantPageBytes = 64 * 1024;

// How much upload space a recording is given at a time. A frame uses well under
// a megabyte between its uniforms and its instance data, so the common case is
// one chunk created once and refilled for the rest of the run.
constexpr std::size_t vulkanUploadChunkBytes = 1024 * 1024;

// Descriptor sets one pool holds. A dispatch takes one, so this is dispatches
// per recording before a second pool is added - which is not a failure, only an
// allocation.
constexpr std::uint32_t vulkanSetsPerDescriptorPool = 64;

// What std140 rounds a uniform block to, taken from the layout the emitter
// wrote the block with. The CPU packs the same block to its widest member
// (Codegen/UniformLayout.h: std140BlockSize is this rounding applied to a type
// list), so a block ending on a float is up to twelve bytes shorter on this
// side than the shader declares it - and a descriptor range shorter than the
// declared block is a validation error at the dispatch rather than at the bind.
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

// EACP_VK_SOFTWARE=1 takes a CPU device over the hardware one, mirroring
// EACP_D3D12_WARP. It is the same debugging affordance: lavapipe is a
// conformant reference, so an app that misbehaves on a GPU and behaves on it
// has found a driver bug rather than its own. It is also what the CI lane sets,
// where lavapipe is the only device there is.
bool prefersSoftwareDevice()
{
    return vulkanEnvironmentFlag("EACP_VK_SOFTWARE");
}

// EACP_VK_VALIDATION=1 turns on VK_LAYER_KHRONOS_validation and a debug-utils
// messenger that logs what it says. Off by default because the layer costs
// several times the driver's own time per call; on, it is the only way to see
// that a descriptor was never written or a barrier never recorded.
bool wantsValidation()
{
    return vulkanEnvironmentFlag("EACP_VK_VALIDATION");
}

std::uint64_t currentThreadId()
{
    return static_cast<std::uint64_t>(
        std::hash<std::thread::id> {}(std::this_thread::get_id()));
}

// Preference order for a device when nothing has asked for a particular one:
// discrete, then integrated, then a virtualised GPU, then a CPU. The last is
// last because it is two orders of magnitude slower and never what a machine
// with anything else should pick - EACP_VK_SOFTWARE is how it is asked for.
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

VKAPI_ATTR VkBool32 VKAPI_CALL
    vulkanDebugCallback(VkDebugUtilsMessageSeverityFlagBitsEXT,
                        VkDebugUtilsMessageTypeFlagsEXT,
                        const VkDebugUtilsMessengerCallbackDataEXT* data,
                        void*)
{
    if (data != nullptr && data->pMessage != nullptr)
        LOG("Vulkan: ", data->pMessage);

    // False is what the layer is told to do next, and it means "carry on":
    // returning true aborts the call that was being validated, which turns a
    // report into a second, different failure.
    return VK_FALSE;
}

// The floor the backend is written against: Vulkan 1.3 core, plus the five
// features it uses that are not on by default. Each is asked for by name rather
// than assumed, so a device without one leaves Device::isValid() false instead
// of failing at the first dispatch.
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

// A family that can do both, so one queue serves render and compute and a
// resource never has to change hands between them. Compute-only is accepted as
// a fallback: everything stage 2 records is a dispatch or a copy.
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

// The two vkCreate calls both layouts end in, and the one flag they both carry.
//
// Partially bound, so a shader that binds three of the eight buffer slots leaves
// the other five unwritten instead of needing a dummy descriptor each - which is
// what the D3D12 backend has to do for Tier 1 hardware (bindComputeRootState)
// and what this feature exists to avoid.
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

// One of the four sampling configurations as a VkSampler, decoded from the
// index rather than from a TextureSampling so the loop that builds them is the
// one place that has to agree with samplingIndex's packing.
//
// Mip filtering follows the same filter, which is what both other backends do:
// a Linear slot samples between levels as well as within one, and a Nearest
// slot - pixel art, a mask, an index texture - gets neither. maxLod is
// unbounded so a texture's whole chain is reachable; one with a single level
// clamps to it on its own.
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

// ------------------------------------------------------------- SPIR-V reading

VulkanTextureBindings spirvTextureBindings(const Vector<std::uint32_t>& words,
                                           int firstBinding)
{
    auto bindings = VulkanTextureBindings {};

    // Magic number, version, generator, id bound, schema. The bound is one past
    // the largest <id> the module uses, which is what the tables below are
    // sized by - SPIR-V ids are dense and start at 1, so an array indexed by id
    // is the cheapest map there is.
    constexpr auto headerWords = 5;

    if (words.size() <= headerWords)
        return bindings;

    constexpr auto opTypeImage = std::uint32_t {25};
    constexpr auto opTypeSampledImage = std::uint32_t {27};
    constexpr auto opTypePointer = std::uint32_t {32};
    constexpr auto opVariable = std::uint32_t {59};
    constexpr auto opDecorate = std::uint32_t {71};
    constexpr auto decorationBinding = std::uint32_t {33};

    // OpTypeImage's Sampled operand: 1 is an image that will be read through a
    // sampler, 2 one a shader reads or writes with the image instructions. The
    // emitter produces exactly two shapes - a `sampler2D`, which is an
    // OpTypeSampledImage over a Sampled=1 image, and a `writeonly image2D`,
    // which is a bare Sampled=2 image - so this is the operand that separates a
    // COMBINED_IMAGE_SAMPLER from a STORAGE_IMAGE.
    constexpr auto sampledThroughASampler = std::uint32_t {1};

    const auto bound = static_cast<int>(words[3]);

    if (bound <= 0)
        return bindings;

    // What each id turned out to be. Three parallel tables rather than a struct
    // per id, because two of them are only ever read for a handful of ids and
    // the third for one.
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

    // Result id and result *type* id of every module-scope OpVariable that
    // carries a Binding decoration in range, paired with the slot it names.
    // Collected rather than resolved inline because a valid module puts its
    // annotations ahead of its types, so the pointer a variable's type names is
    // not known yet when the decoration is read.
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

        // A zero-length instruction cannot be stepped over, and a length past
        // the end means the module is not what it says it is. Either way there
        // is nothing further to read, and answering with what was found so far
        // leaves the failure to the driver, which has a better message for it.
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

        // A binding in the texture range that points at neither kind of image -
        // which nothing the emitter writes does - is left undeclared rather
        // than guessed at, so the layout describes only what was recognised.
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

// ---------------------------------------------------------------- the shared

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
    // volkInitialize dlopens libvulkan.so.1. A machine with no loader and no
    // driver stops here, which is the same "no device" answer a Mac without
    // Metal or a PC without D3D12 gives, and every GPU test already self-skips
    // on it.
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

    // One sampler per configuration, made here rather than per texture: a
    // sampling configuration belongs to the shader that declared it, and a
    // combined image sampler descriptor pairs whichever of these the shader
    // asked for with the image being bound. Logged rather than fatal - a driver
    // that cannot make four samplers has larger problems, and the bind sites
    // already drop a texture whose sampler is null.
    for (auto index = 0; index < samplingConfigurations; ++index)
    {
        samplers[index] = makeSampler(device, index);

        if (samplers[index] == VK_NULL_HANDLE)
            LOG("Vulkan: sampler ", index, " could not be created");
    }

    // The 90 ms glslang spends building its built-in symbol tables, paid here
    // rather than by whichever ShaderLibrary happens to be first - which, in an
    // app that builds a pipeline lazily, is a frame.
    Spirv::warmUp();
}

bool VulkanShared::createInstance()
{
    // The instance version is the loader's, and it caps what apiVersion may
    // ask for: a 1.2 loader refuses a 1.3 instance outright. Asking first is
    // what turns an old distribution into "no device" rather than into a
    // failure at vkCreateInstance with nothing to say about it.
    //
    // A 1.0 loader has no vkEnumerateInstanceVersion at all, which under volk
    // is a null function pointer rather than a link error - the same answer,
    // asked before the call rather than by it.
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

    VkInstanceCreateInfo info = {};
    info.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    info.pApplicationInfo = &application;
    info.enabledLayerCount = static_cast<std::uint32_t>(layers.size());
    info.ppEnabledLayerNames = layers.data();
    info.enabledExtensionCount = static_cast<std::uint32_t>(extensions.size());
    info.ppEnabledExtensionNames = extensions.data();

    if (vkCreateInstance(&info, nullptr, &instance) != VK_SUCCESS)
        return false;

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

        // EACP_VK_SOFTWARE inverts the order rather than filtering: a machine
        // whose only device is a real GPU still gets one, so the switch is
        // never the reason a test finds no device.
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

    // Exactly the five probeFeatures asked about, and nothing else: enabling a
    // feature the backend does not use costs driver state and hides the day one
    // of them stops being available.
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

    VkDeviceCreateInfo info = {};
    info.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    info.pNext = &enabled;
    info.queueCreateInfoCount = 1;
    info.pQueueCreateInfos = &queueInfo;

    if (vkCreateDevice(physicalDevice, &info, nullptr, &device) != VK_SUCCESS)
    {
        device = VK_NULL_HANDLE;
        return false;
    }

    // One device in the process, so the device-level dispatch table can be the
    // global one volk loads here rather than a table per device.
    volkLoadDevice(device);
    vkGetDeviceQueue(device, queueFamily, 0, &queue);

    return queue != VK_NULL_HANDLE;
}

bool VulkanShared::createAllocator()
{
    // The volk recipe: VMA is given the two entry points that find every other
    // one, rather than linking against symbols that do not exist under
    // VK_NO_PROTOTYPES.
    VmaVulkanFunctions functions = {};
    functions.vkGetInstanceProcAddr = vkGetInstanceProcAddr;
    functions.vkGetDeviceProcAddr = vkGetDeviceProcAddr;

    VmaAllocatorCreateInfo info = {};
    info.vulkanApiVersion = VK_API_VERSION_1_3;
    info.instance = instance;
    info.physicalDevice = physicalDevice;
    info.device = device;
    info.pVulkanFunctions = &functions;

    // Sub-allocation rather than one VkDeviceMemory per buffer, which is the
    // whole reason VMA is here: maxMemoryAllocationCount is commonly 4096, and
    // the committed-resource model D3D12 uses would run a scene out of
    // allocations long before it ran out of memory.
    return vmaCreateAllocator(&info, &allocator) == VK_SUCCESS;
}

PipelineLayouts makeComputeLayouts(VkDevice device,
                                   const VulkanTextureBindings& textures)
{
    // The set a kernel binds, laid out exactly as Codegen/ShaderBindings.h
    // prints it: storage buffers from binding 0, textures from
    // ComputePass::textureRegisterBase, the uniform block above both.
    //
    // The uniform block is a UNIFORM_BUFFER_DYNAMIC so a recording's worth of
    // dispatches share one constant page and differ only in the offset handed
    // to vkCmdBindDescriptorSets.
    //
    // Only the texture slots the module actually declares get a binding, and
    // each gets the type it was declared with - a sampler2D is a
    // COMBINED_IMAGE_SAMPLER and a writeonly image2D a STORAGE_IMAGE, and one
    // binding cannot be both. That is why the texture half of this is per
    // pipeline where the rest is shared; see VulkanTextureBindings. A slot the
    // kernel never named has no binding here, and a bind to it is dropped by
    // the pass rather than written into a descriptor the shader cannot read.
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
    // The set a graphics pipeline binds, laid out exactly as
    // Codegen/ShaderBindings.h prints it for a render shader: the uniform block
    // at vulkanUniformBinding, the maxTextureSlots textures above it, the
    // storage buffers from RenderPass::bufferBase. The compute set is the same
    // three kinds at different numbers, which is the whole reason there are two.
    //
    // Every binding is visible to both stages, because one GLSL global is one
    // binding whichever stage reads it: the emitter writes the uniform block,
    // the samplers and the buffer blocks outside the EACP_VERTEX / EACP_FRAGMENT
    // guards, so the vertex and fragment modules of one program declare the same
    // numbers and a set written once serves both.
    //
    // The samplers are not immutable. A combined image sampler with
    // pImmutableSamplers set would pin the filtering into the *layout*, and the
    // sampling a slot wants is a property of the texture bound into it - so it
    // would need a layout, and therefore a pipeline layout, per sampling
    // combination a shader happens to declare. The sampler travels with the
    // image in the descriptor write instead.
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
    // The layout every kernel that declares no texture binds through, which is
    // most of them - built once here rather than per pipeline. A kernel that
    // does declare one needs a layout of its own, the descriptor type of a
    // texture binding being a property of the module rather than of the binding
    // map; see ComputePipeline-Linux.cpp.
    computeLayouts = makeComputeLayouts(device, {});

    return computeLayouts.isValid();
}

VulkanShared& getVulkanShared()
{
    static auto shared = VulkanShared();
    return shared;
}

// --------------------------------------------------------------- the context

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

    // Everything owed a release goes now: waitIdle has already run, so nothing
    // the GPU is still reading is among it.
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

        // The command buffer goes with the pool it came from, which is what a
        // pool per recording is for.
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

        // Resetting the pool rather than the buffer returns the command memory
        // to the pool instead of leaving it fragmented across recordings, which
        // is the whole reason there is a pool per recording rather than one
        // pool with many buffers.
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

std::uint64_t VulkanContext::submit(CommandContext* commands)
{
    assertOwningThread();

    if (commands == nullptr || !isValid())
        return 0;

    // One global barrier at the end of every recording, which is what makes the
    // per-recording use tracking in transitionForUse correct: consecutive
    // submissions on a queue execute in order but are not automatically visible
    // to each other, so without this a buffer written by one dispatch and read
    // by the next submission would need a barrier nobody is in a position to
    // record. One barrier per submit is a rounding error against the dozens a
    // frame would otherwise pay.
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
        // An invalid recording must not execute. Nothing reached the GPU, so
        // its pooled slots are free at once rather than behind a value.
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

    VkSemaphoreSubmitInfo signal = {};
    signal.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO;
    signal.semaphore = timeline;
    signal.value = value;
    signal.stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;

    VkSubmitInfo2 submitInfo = {};
    submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2;
    submitInfo.commandBufferInfoCount = 1;
    submitInfo.pCommandBufferInfos = &bufferInfo;
    submitInfo.signalSemaphoreInfoCount = 1;
    submitInfo.pSignalSemaphoreInfos = &signal;

    {
        // The queue is the one thing a Device does not own (see the note in
        // VulkanContext.h), and a VkQueue is externally synchronized.
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

// Once per process, so a recording the driver refuses is a line in the log
// rather than a dispatch that quietly went missing.
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

    // A counter that cannot be read is a lost device, and answering "complete"
    // for it is deliberate: nothing queued will ever finish, so holding on to
    // a recording or a retired object for it would hold it forever, and every
    // waitFor would spin. Recovery is not attempted here; GPUView's
    // onDeviceRestored is the seam for it, and until it is wired the objects
    // freed on the way out were not going to be used again.
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

// The newest value this context signalled, which the queue being FIFO makes the
// same thing as everything it ever submitted. There is nothing to signal for a
// context that has submitted nothing.
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
    // The callbacks fire after the pending list has been rebuilt rather than
    // during the walk: one of them is free to commit more work, which appends
    // to the very vector being walked.
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

    // Unstamped, because the value that frees it is not knowable yet. Every
    // command buffer that can name the object from here on is one that already
    // exists - no new command can name it, its owner is gone - but one of those
    // may still be recording, and an open recording has no completion value
    // until it submits. purgeRetired does the stamping once nothing is
    // recording.
    //
    // Stamping it here with the value the next submit will carry is the version
    // that is wrong, and it is wrong in a way that took the D3D12 backend a
    // crash to find: an upload issued during a frame acquires a recording of
    // its own that signals *ahead* of the frame's, so the stamp completes while
    // the recording still naming the object is open.
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
    // Nothing is recording, so every command buffer that could name anything
    // retired so far has been submitted, and lastSubmittedValue is at or past
    // all of their values. That is the first moment an entry can be given a
    // value that is sound, and it is why the stamping is here rather than at
    // the point of retirement.
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

    // Each goes as its own value passes. Waiting instead for the timeline to be
    // past *everything* ever submitted is the other version that is wrong: under
    // continuous rendering the counter is always a frame or two behind, so it
    // frees nothing at all and the working set grows by megabytes a second until
    // the app happens to fall idle.
    retired.eraseIf(
        [this](Retired& entry)
        {
            if (!entry.stamped || !hasCompleted(entry.completionValue))
                return false;

            entry.destroy();
            return true;
        });
}

// ------------------------------------------------------------ host-side memory

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

    // Coherent is required rather than flushed by hand. A flush per upload is
    // one more thing every call site would have to remember, and every device
    // has a host-visible coherent type - it is the one Vulkan guarantees.
    // Cached memory is asked for on the readback side, where the CPU reads what
    // the GPU wrote and write-combined memory is an order of magnitude slower
    // to read than to write.
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
    // Forward only. A chunk the cursor has passed was too full for an earlier
    // request, and going back to check it again on every upload would make this
    // linear in the uploads a frame has already made.
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
    // The page this recording is already filling, while it still has room. A
    // uniform block is a couple of hundred bytes and a recording's dispatches
    // run into the hundreds at most, so this is the answer nearly every time.
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

    // Two roundings, and they are different questions. The range is what the
    // shader's block is - std140 rounds it to 16 - and the step is where the
    // next block may start, which the device's dynamic-offset alignment decides.
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

    // A free slot already big enough is the common case once the traffic
    // settles: every frame of a given clip, and every run of a given model,
    // moves exactly the same number of bytes.
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

    // Otherwise grow a free slot rather than adding one, so a stream that
    // switches to a larger frame size does not strand the old buffers.
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

// ---------------------------------------------------------------- descriptors

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

    // Forward only, on the same terms as the upload arena: a pool the cursor
    // has passed was full for an earlier request and will not have become
    // emptier since - nothing is freed from one until the whole recording is
    // recycled.
    while (commands.descriptorCursor < commands.descriptorPools.size())
    {
        if (auto set =
                allocateFrom(commands.descriptorPools[commands.descriptorCursor]))
            return set;

        ++commands.descriptorCursor;
    }

    // Both image types at the full texture width, because which of the two a
    // slot takes is decided per shader (VulkanTextureBindings) and a pool is
    // shared by every set a recording allocates. The overcount is descriptor
    // headroom in a pool that is reset with the recording, not memory.
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
